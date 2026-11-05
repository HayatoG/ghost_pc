# Rasterizador de GS em faixas (PS2X_GS_BANDS) — estado em 05/11/2026

Escopo: `gs_banded_backend.h/.cpp` (reescrito), `gs_thread_backend.cpp` (log `bands.*`,
`PS2X_GS_BANDS=auto`), `gs_backend_factory.cpp` (alias `PS2X_GS=cpu-serial`), `tools/regress.ps1`
(`-Renderer` aceita `cpu-serial|cpu-thread|hw`), ferramenta nova `tools/gsbands/` (harness de replay).
Sucede a seção 3 de `gs-threading.md`. Padrão continua **desligado** (`cpu` = serial; faixas só com
`PS2X_GS=cpu-thread PS2X_GS_BANDS=N|auto`).

## Estado

[PREENCHER: resultado final da medição e da regressão; ver "Medição" abaixo]

## Design

- Despachante = thread de GS do `cpu-thread` (o guest só enfileira na fila de 32 MiB; as faixas não o
  bloqueiam). N workers, cada um com seu `GSCpuBackend` sobre a mesma VRAM, dormindo em
  `condition_variable` (sem busy-wait; o despachante só acorda worker cuja caixa estava vazia, o worker
  só notifica quando o despachante espera). `auto` = min(8, threads de hardware − 4).
- Partição: faixas de `PS2X_GS_BAND_ROWS` (32) linhas de tela, faixa s → worker s % N. Cada worker
  processa **todos** os draws na ordem, só nas suas linhas (scissor recortado). Pixels de FB/Z de 16/32
  bits não dividem bytes; cada faixa de 32 linhas cobre linhas pares→ímpares inteiras de uma coluna de
  bloco (64 B = 8x2 px em CT32), então também não há falso compartilhamento de linha de cache.
- Por que não "página mod N": FB e Z do mesmo pixel caem em páginas diferentes (e de alturas
  diferentes, CT32 x Z16), e o teste de Z exige que o mesmo worker faça Z e cor do pixel; logo o dono
  tem de ser o pixel. A consistência entre draws é garantida por uma **assinatura** gravada por página
  suja: (classe de layout, largura, base mod largura, fase = (base/largura x altura da página) mod
  (linhas x N)). Mesma assinatura ⇒ todo byte compartilhado tem o mesmo dono e é tocado na ordem
  original. Assinatura diferente ⇒ barreira.
- Rastreamento por **página** (bitsets de 512): `m_dirty`+assinatura (FB sempre; Z se escrito ou
  testado) e `m_read` (texturas e CLUT). Barreira (todos drenam; caches de textura invalidados) só em:
  textura/CLUT lendo página suja (RAW), escrita/Z em página lida (WAR), assinatura diferente,
  transferência sobre página usada, Present/Clear/leituras. Upload host→local e cópia local→local em
  páginas limpas rodam no mestre **sem barreira** (rechecado a cada `UploadImage`).
- Páginas de textura vêm do **retângulo de texels dos vértices** (envoltória convexa; STQ só com q de
  mesmo sinal), com margem, depois do modo de wrap (repeat, clamp, region clamp, region repeat:
  `(c & MSK) | FIX` fica em [FIX, min(b+FIX, MSK|FIX)]). Sem isso o region repeat marcava 1 022 linhas
  e quase tudo virava serial.
- Draw que amostra o próprio alvo (efeito que lê a região do Z como CT16 enquanto escreve Z; downsample
  que lê o FB como T8) ou cujo x passa da largura do buffer roda **inteiro no worker 0**; draws assim
  consecutivos encadeiam sem barreira ("modo serial"); a próxima operação de outro tipo fecha o modo.
- Cache de textura de 1 página do `GSCpuBackend`: pode ficar velho após escrita sem TEXFLUSH; as faixas
  têm semântica "sempre fresca". `gsbands fresh` mostrou que nas gravações isso não muda a saída
  (serial == serial com flush antes de todo draw texturizado/CLUT).

## Bugs achados e corrigidos (por que divergia)

1. O mestre executava os draws "auto-amostrados" **sem nunca ter recebido `LoadClut`** (CLUT velho):
   hoje esses draws rodam no worker 0, que tem o CLUT.
2. Extensões grosseiras (FB/Z do topo até o fim do scissor, texturas com mip/region = VRAM inteira):
   quase todo draw caía em barreira. Hoje: retângulos de páginas exatos.
3. Mesma largura/formato com base deslocada de um número de linhas não múltiplo de linhas x N: bytes
   com donos diferentes sem barreira (corrida possível) → assinatura com fase.
4. **Passo de linha de páginas de T8/T4** (páginas de 128 px: TBW/2 páginas por linha) contado como TBW:
   páginas reais não marcadas → corrida. Era a divergência do jogo (42 de 480 verificações, páginas
   44-64 e 111) que **não aparecia no replay**; depois da correção, 0.
5. Nota de build: mudar `gs_banded_backend.h` não recompila `gs_thread_backend.cpp`/`gs_backend_factory.cpp`
   (dependência de header não rastreada) → ODR e crash no início. Tocar esses .cpp ao mudar o header.

## Verificação

- Replay (`tools\gsbands\build.cmd`; `work\gsbands\gsbands.exe compare <rec> <workers> [linhas]`, VRAM
  comparada em cada Present e no fim): `scene3d_7570` (84 mil comandos; 4/32, 7/16, 8/32),
  `scene3d_7558` (169 mil; 3/32), `scene3d_7501` (819 mil, 64 verificações; 8/32): **idênticos**.
  (7501/7558 rodados antes da correção 4; 7570 também depois.)
- Jogo, verificador `PS2X_GS_BANDS_VERIFY=1` (sombra serial, compara a VRAM inteira a cada Present),
  `regress_scene3d` inteiro com `cpu-thread` + `auto` (8 workers): **0 divergências** em ~480
  verificações (02:02).
- `regress.ps1 -Renderer cpu-thread`: as capturas diferem da referência **também por temporização**: com
  GS assíncrono a captura sai em outro vsync (q07300 capturado em 7316/7326, menu em 695 em vez de 600).
  Ver resultado com `PS2X_GS_BANDS=0` abaixo para separar o efeito das faixas do da thread.
- Ferramentas: `PS2X_GS_BANDS_DEBUG=1` (motivo dos draws inteiros), `PS2X_GS_DEBUG=1` (contadores
  `bands.*`). Para gravar `.rec`, usar `PS2X_GS=cpu-serial`.

## Números do replay (3 quadros de `scene3d_7570`)

Depois das correções, 8 workers: draws em faixa 74 429, inteiros 566, barreiras 1 674 (~560/quadro;
WAR 579, RAW textura 82, transferência 126, layout 2, CLUT 2; o resto é modo serial e Present); uploads
sem barreira 230. Tempo (máquina com ~54 % de carga de outros agentes, só indicativo): 4 workers
**1,72x** sobre o serial (0,343 → 0,199 s), medido antes da correção 4.

## Medição no jogo

[PREENCHER]

## Próximos passos

- Granularidade de bloco (256 B) em vez de página para cortar WAR/RAW; testar 16 linhas e 8 workers
  com a máquina quieta; lote de sprites minúsculos por worker.
