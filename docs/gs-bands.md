# Rasterizador de GS em faixas (PS2X_GS_BANDS) — estado em 05/11/2026 ~03:05

Escopo: `gs_banded_backend.h/.cpp` (reescrito), `gs_thread_backend.h/.cpp` (MXCSR do produtor no
`SubmitCmd`, log `bands.*`, `PS2X_GS_BANDS=auto`), `gs_backend_factory.cpp` (`PS2X_GS=cpu-bands` e
`cpu-serial`), `tools/regress.ps1` (`-Renderer cpu-serial|cpu-thread|cpu-bands|d3d11|hw`), ferramenta
nova `tools/gsbands/` (harness de replay). Sucede a seção 3 de `gs-threading.md`.

## Resultado

| Modo (`regress_scene3d`, janela de vsync 7530-7640, `guest_vsync_hz` interpolado) | vsync/s |
|---|---|
| `PS2X_GS=cpu-thread` (thread de GS, sem faixas) | 11,4 |
| `PS2X_GS=cpu-thread PS2X_GS_BANDS=auto` (8 workers) | **18,8** (+65 %) |
| `PS2X_GS=cpu-bands PS2X_GS_BANDS=auto` (faixas sem thread de GS) | 12,0 |

Uma corrida de cada, em sequência, 02:55-03:03, com outros agentes usando a máquina (não estava
quieta). Referência de antes: `cpu` serial 6,06 (`logs/final/novo_cpu.csv`, mesma janela).
CSVs: `logs/gsbands/m_*.csv`; script: modelo do `final_measure.sh` (`PS2X_TARGET_FPS=0`,
`PS2X_PAD_CLOCK=guest`, `PS2X_VIDEO_HZ=50`).

**Correção:**
- Verificador (`PS2X_GS_BANDS_VERIFY=1`, sombra serial, compara a VRAM inteira de 4 MiB a cada
  Present) no `regress_scene3d` inteiro: **0 divergências** em `cpu-thread`+faixas (~480 verificações)
  e em `cpu-bands` (~5 300 verificações, depois da correção 6 abaixo).
- `regress.ps1 -Renderer cpu-bands`: menu IGUAL; scene3d **5/6 IGUAL**, e a 6ª varia entre corridas
  (q07300 numa, q07500 noutra, sempre a mesma imagem alternativa) com o verificador dizendo VRAM
  idêntica em todo Present: é a captura pegando o Present vizinho (temporização da janela, ressalva
  já descrita em `regress.md`), não o desenho.
- `regress.ps1 -Renderer cpu-thread` + `PS2X_GS_BANDS=auto` (03:06, com a correção de MXCSR): menu
  IGUAL; scene3d q07300 e q07500 IGUAIS, as outras 4 capturadas **1 vsync depois** do alvo (7565, 7629,
  7693, 7701: GS assíncrono, ressalva de `regress.md`), por isso diferentes. A equivalência do desenho
  nesse modo é dada pelo verificador (0 divergências), não pelas capturas.
- Replay (`gsbands compare`): `scene3d_7570`, `7558`, `7501` (819 mil comandos) e `7536_cur`
  (gravado hoje do `cpu-serial`) idênticos com 3, 4, 7 e 8 workers.

**Padrão:** continua `cpu` = serial. Recomendo `cpu-thread` + `PS2X_GS_BANDS=auto` como padrão do
caminho de CPU (o ganho é real e o verificador passa), mas a troca do padrão fica para o líder decidir
junto com o `hw` (12,2 vsync/s), já que a regressão por captura do `cpu-thread` tem a ressalva de
temporização.

## Design

- Despachante = thread de GS (`cpu-thread`) ou a própria thread que chama (`cpu-bands`, com mutex na
  API porque a janela também chama `Present`). O guest só enfileira na fila de 32 MiB. N workers, cada
  um com seu `GSCpuBackend` sobre a mesma VRAM, dormindo em `condition_variable` (sem busy-wait; o
  despachante só acorda worker cuja caixa estava vazia, o worker só notifica quando alguém espera).
  `auto` = min(8, threads de hardware − 4).
- Partição: faixas de `PS2X_GS_BAND_ROWS` (32) linhas de tela, faixa s → worker s % N. Cada worker
  processa **todos** os draws na ordem, só nas suas linhas (scissor recortado). Pixels de FB/Z de 16/32
  bits não dividem bytes; uma faixa de 32 linhas cobre pares de linhas inteiros de cada coluna de bloco
  (64 B = 8x2 px em CT32), então não há falso compartilhamento de linha de cache.
- Por que não "página mod N": FB e Z do mesmo pixel caem em páginas diferentes (e de alturas
  diferentes, CT32 x Z16) e o teste de Z exige que o mesmo worker faça Z e cor do pixel; logo o dono é
  o pixel. A consistência entre draws vem de uma **assinatura** gravada por página suja: (classe de
  layout, largura, base mod largura, fase = (base/largura x altura da página) mod (linhas x N)). Mesma
  assinatura ⇒ todo byte compartilhado tem o mesmo dono e é tocado na ordem original.
- Rastreamento por **página** (bitsets de 512): `m_dirty`+assinatura (FB sempre; Z se escrito ou
  testado) e `m_read` (texturas e CLUT). Barreira (todos drenam; caches de textura invalidados) só em:
  textura/CLUT lendo página suja (RAW), escrita/Z em página lida (WAR), assinatura diferente,
  transferência sobre página usada, Present/Clear/leituras. Upload host→local e cópia local→local em
  páginas limpas rodam no mestre **sem barreira** (rechecado a cada `UploadImage`).
- Páginas de textura vêm do **retângulo de texels dos vértices** (envoltória convexa; STQ dividido pelo
  q com sinal, como o rasterizador; só com q de mesmo sinal), com margem, depois do modo de wrap
  (repeat, clamp, region clamp, region repeat: `(c & MSK) | FIX` fica em [FIX, min(b+FIX, MSK|FIX)]).
- Draw que amostra o próprio alvo (efeito que lê a região do Z como CT16 enquanto escreve Z; downsample
  que lê o FB como T8) ou cujo x passa da largura do buffer roda **inteiro no worker 0**; draws assim
  consecutivos encadeiam sem barreira ("modo serial"); a próxima operação de outro tipo fecha o modo.
- Cada draw leva o **MXCSR** de quem o enviou e o worker o aplica (ver correção 6).

## Bugs achados e corrigidos (por que divergia)

1. O mestre executava os draws auto-amostrados **sem nunca ter recebido `LoadClut`**: hoje eles rodam no
   worker 0, que tem o CLUT.
2. Extensões grosseiras (FB/Z do topo até o fim do scissor; texturas com mip/region = VRAM inteira):
   quase todo draw caía em barreira. Hoje: retângulos de páginas.
3. Mesma largura/formato com base deslocada de um número de linhas não múltiplo de linhas x N: bytes
   com donos diferentes sem barreira → assinatura com fase.
4. **Passo de linha de páginas de T8/T4** (páginas de 128 px: TBW/2 páginas por linha) contado como TBW:
   páginas reais não marcadas → corrida (42 de 480 verificações no jogo; não aparecia no replay).
5. Mudar `gs_banded_backend.h`/`gs_thread_backend.h` não recompila os .cpp que os incluem (dependência
   de header não rastreada pelo ninja daqui) → ODR e crash no boot. Tocar os .cpp ao mudar os headers.
6. **Modo de arredondamento.** O VU1 roda com `fesetround(FE_TOWARDZERO)` e os draws de PATH1 (XGKICK)
   chegam ao backend **dentro** dessa execução: o serial rasteriza com arredondamento para zero, os
   workers (threads novas) com o padrão. Diferenças de 1 ulp em Z/cor/coordenada de textura (erro de
   até 9 por canal a partir do vsync 7540). Corrigido levando o MXCSR no item de trabalho e no
   `SubmitCmd` do `cpu-thread`. **O mesmo bug existia no `cpu-thread` sem faixas** (por isso o
   `regress scene3d` dele diferia a partir de q07564): agora a thread de GS aplica o MXCSR do produtor.
   O replay não pegava porque roda tudo com o MXCSR padrão.

Diagnóstico que levou a isso: `PS2X_GS_BANDS_VERIFY=2` + `PS2X_GS_BANDS_VERIFY_FROM=<vsync>` (compara
após cada draw, só a partir daquele vsync) mostrou divergência num triângulo isolado com VRAM idêntica
antes dele; `PS2X_GS_BANDS_SHADOW_FRESH=1` (sombra sem cache velho) não mudou nada, então não era cache.

## Números do replay (3 quadros de `scene3d_7570`, 8 workers)

Draws em faixa 74 429, inteiros 566, barreiras 1 674 (~560/quadro; WAR 579, RAW textura 82,
transferência 126, layout 2, CLUT 2; o resto é modo serial e Present); uploads sem barreira 230.
`gsbands bench` 4 workers 1,72x sobre o serial (máquina carregada; medido antes da correção 4).

## Ferramentas

`tools\gsbands\build.cmd` → `work\gsbands\gsbands.exe compare <rec> <workers> [linhas] [every]`
(VRAM comparada em cada Present e no fim), `bench`, `fresh`. No jogo: `PS2X_GS_BANDS_VERIFY=1|2`,
`PS2X_GS_BANDS_VERIFY_FROM`, `PS2X_GS_BANDS_DEBUG=1` (motivo dos draws inteiros), `PS2X_GS_DEBUG=1`
(contadores `bands.*`). Para gravar `.rec`, usar `PS2X_GS=cpu-serial`.

## Próximos passos

- Decidir o padrão (`cpu-thread` + `auto`), e o launcher passar `PS2X_GS_BANDS`.
- Cortar barreiras WAR/RAW com granularidade de bloco (256 B); testar 16 linhas; agrupar sprites
  minúsculos por worker; medir com a máquina quieta.
