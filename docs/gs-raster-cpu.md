# Rasterizador de GS em CPU — fase 4 (04/11/2026): cena 3D de gameplay

Escopo: `gs_cpu_backend.cpp/.h`, `gs_texture_page_cache.h`, `ps2_gs_memory.cpp/.h` (PS2Recomp, GPL;
árvore `work/ps2recomp_game/ps2xRuntime`). Continuação de `gs-optimization.md` (fases 1–3). Perfil de
partida: `gameplay-profile-2026-11-04.md` (rasterizador ≈ 52 % do host na cena 3D).

## Resultado

Medido no **harness de replay** (abaixo), mesmo stream de comandos GS gravado do jogo (3 quadros de
gameplay, vsync 7570–7573: 84 104 comandos, 55 268 sprites, 21 112 triângulos), backend original x novo
rodando em *lockstep* na mesma thread (ciclos de thread, `QueryThreadCycleTime`), máquina carregada por
outros agentes (por isso medida relativa, não absoluta):

| Categoria | Original (Mcyc) | Novo (Mcyc) | Ganho |
|---|---|---|---|
| Sprites | 1 330 | 564 | **2,36x** |
| Triângulos | 859 | 450 | **1,91x** |
| Total (inclui uploads/CLUT, ~5 %) | 2 294 | 1 122 | **2,04x** |

Evolução (total, mesmo método): 1,40x (CLUT expandido + leitores inline + setup em cache + triângulo
SIMD) → 1,47x (endereço de FB/Z separável) → 1,56x (tabelas compactas) → 1,62x (Z inline sem lambda,
`NotifyWrite` inline) → 1,72x (escrita simples) → 1,97x (blend SIMD + escrita com blend) → 2,04x
(wrap/endereço de textura vetorizados no triângulo). FPS do jogo: **não medido** por mim (regra do
líder); o rasterizador era ~52 % da thread do jogo, então o teto do ganho no quadro é ~1,35x.

**Correção:** saída byte-idêntica em todas as etapas:
- harness `compare` (VRAM inteira comparada a cada 1–64 comandos, original x novo): streams reais
  vsync 7501–7577 (818 934 comandos), 7558–7566, 7570–7573, e 12 gravações sintéticas (`fuzz`, 30–40 mil
  primitivas cada, todos os formatos de textura/frame/Z, CLUT CSM1/CSM2, TEXA, fog, AFAIL, DATE, FBA,
  FBMSK, blend com todos os seletores, Z24/Z32/Z16S, uploads e cópias local→local);
- `tools/regress.ps1 -Scenario menu`: IGUAL a cada build;
- `tools/regress.ps1 -Scenario scene3d`: 6/6 IGUAL no 1º build; depois houve uma regressão em
  q07564+ que **não era do rasterizador** (prova: build da mesma árvore com os 5 arquivos deste escopo
  trocados pelos originais deu os mesmos MD5 errados; causa achada pelo líder na correção de GIFtag de
  outro agente, hoje desligada por padrão). Resultado final: ver "Estado" no fim.

## Ferramentas novas

### Gravador de comandos do backend (`PS2X_GS_RECORD`)
No `GSCpuBackend` (custa um teste de ponteiro por chamada quando desligado):

| Variável | Efeito |
|---|---|
| `PS2X_GS_RECORD=<arquivo>` | liga o gravador |
| `PS2X_GS_RECORD_VSYNC=<n>` | começa no 1º `Present` com `vsyncTick >= n` |
| `PS2X_GS_RECORD_PRESENTS=<k>` | grava `k` presents e fecha o arquivo |

Formato: cabeçalho (`GSRC`, versão, `sizeof(GSPrimitiveBatch)`), VRAM de 4 MB, CLUT, estado do
`TexturePageCache`, estado de transferência; depois registros `u8 tipo, u32 tamanho, payload` para
`Submit`, `LoadClut`, `BeginTransfer`, `UploadImage`, `TextureFlush`, `ClearFramebuffer`, `WriteVram`,
`ConsumeLocalToHostBytes`, `Reset` e marcadores de `Present`. `GSCpuBackend::RestoreRecordedState`
reconstrói o estado. **Os `.rec` contêm dados do jogo (VRAM, texturas): só em `work/gsrec/`, nunca no git.**
Exemplo (3 quadros de gameplay, ~33 MB):
```
$env:PS2X_GS_RECORD="...\ghost_pc\work\gsrec\scene3d_7570.rec"; $env:PS2X_GS_RECORD_VSYNC="7570"; $env:PS2X_GS_RECORD_PRESENTS="3"
powershell -NoProfile -File tools\regress.ps1 -Scenario scene3d -Exe work\raster_exe\ps2EntryRunner.exe
```
(O `regress.ps1` herda as variáveis; a regressão continua valendo — gravar não muda a imagem.)

### `gsreplay` (`tools/gsreplay/`, fonte versionado)
Compila o backend **original congelado** (cópia em `work/raster_baseline_src/`, fora do git; classe
renomeada por macro para `GSCpuBackendOld`/`GSMemOld`) e o **atual da árvore** no mesmo executável, com as
mesmas flags do `build_perf`. `tools\gsreplay\build.cmd [dir_baseline] [newonly]` → `work\gsreplay\gsreplay.exe`.
Comandos: `compare` (A/B byte a byte, mostra o 1º comando divergente e pixels), `abstep` (tempo A/B em
lockstep), `bench`, `profile` (amostrador por linha de código + `rips.txt` para anotar o disassembly),
`stats`/`cfgprof` (histograma de estados de desenho por área / por ciclos), `diffstreams` (1º comando
diferente entre duas gravações, p.ex. de dois executáveis), `fuzz` (gravação sintética).
Ciclo típico: editar → `build.cmd . newonly` (~1 min) → `compare` em `fuzz*.rec` e nos `.rec` reais →
`abstep`. Só depois o build do jogo e o `regress.ps1`.

## Mudanças (e por quê)

1. **CLUT expandido** (`PrepareClutTable`): a tabela de 256 (ou 16) cores RGBA32 finais (com TEXA) é
   montada uma vez por combinação (versão do CLUT, CPSM, CSA, CSM, T4/T8, TEXA) e reaproveitada; o texel
   T8/T4 vira um acesso a tabela. `m_clutVersion` sobe a cada `LoadClutUnlocked`/`Reset`.
2. **Leitores de textura inline** com `ResolveDeferred` para todos os formatos (antes só CT32; os outros
   iam por ponteiro de função e pelo `Resolve`, que copiava 8 KB a cada troca de página).
3. **Cache de setup por estado**: `PixelSetup`/`TextureSetup` só são refeitos quando o `GSDrawState`
   (memcmp) ou a versão do CLUT mudam — a maioria dos triângulos é minúscula (mediana 4,6 px).
4. **Templates por formato de textura** (`kTexC32/C24/P8/P4/Generic`) nos laços de sprite e triângulo.
5. **Sprite**: a parte U da amostragem (incluindo wrap e, para CT32/CT24, o termo de swizzle) é calculada
   uma vez por coluna e a parte V uma vez por linha (eram por pixel, com duas divisões).
6. **Triângulo em SIMD, 4 pixels por vez**: funções de aresta, Z (double, AVX), cor, fog, STQ/UV, 1/q,
   floor e — para os formatos rápidos — wrap e a parte do endereço que depende das coordenadas. Cada
   lane faz **exatamente** a sequência escalar original; o trabalho com memória (textura, FB, Z) continua
   escalar e na ordem original de pixels (a ordem dirige o cache de página de textura).
7. **Endereço separável**: para CT32/Z32/Z16S a tabela de swizzle é `tb[bloco] + tx[x] + ty[y]`
   (verificado na inicialização para as 32×páginas inteiras); FB e Z têm o termo de coluna por coluna e o
   de linha por linha. Para T8/T4/CT32 de textura, **tabela compacta** `tb[bloco] + T0[y][x]` (1 página:
   4–32 KB em vez de 128–512 KB, cabe em L1). Aritmética mod 2^32 idêntica ao `swizzledByteAddress`.
8. **WritePixel**: endereços de FB/Z vêm do chamador; leitura de Z inline (o lambda que capturava por
   referência **não era inlinado** pelo MSVC: chamada + closure por pixel); Z32/Z24 inline;
   `NotifyWrite` force-inline com a cópia rara fora de linha; `combineTexturePacked` (o struct de 4
   bytes ida-e-volta pela pilha causava *store-forwarding stall* por pixel); **blend em SIMD** (3 canais,
   mesma aritmética inteira com `>>7` aritmético e clamp); caminhos `WritePixelSimple` (sem teste/blend:
   só FBA, FBMSK e as escritas) e `WritePixelSimpleBlend` (idem + blend), escolhidos por primitiva.

### A armadilha do `/fp:fast` (por que precisei ler o disassembly)
Com `/fp:fast /arch:AVX2` o MSVC contrai `a*b + c*d` em FMA e **escolhe qual produto funde por contexto**.
Só de mover o cálculo do denominador do triângulo para outra função ele passou a fundir o outro produto:
1 ulp de diferença em `1/|denom|` → Z diferente no último bit (o `fuzz` pegou no registro 310). Solução:
li o disassembly do original e fixei a mesma sequência com intrínsecos (`_mm_fmadd_ss/ps/pd`): produto
"de linha" arredondado, produto "de pixel" fundido; atributos `a0*w0` arredondado e os outros dois
fundidos da esquerda para a direita; `(1 - w0) - w1`; `1/q` exato; `(invQ*s)*texW`. Intrínsecos não são
recontraídos, então o resultado deixa de depender do humor do compilador.

## Tentado / não feito
- **Inline do `WriteP8` no `UploadImage`**: ficou *mais lento* (26 %) no A/B — revertido.
- `memset`/`rep stos` para sprite de cor sólida: o caso real tem FBMSK ou blend; ficou o
  `WritePixelSimple` (que já é uma escrita por pixel sem testes).
- Especializar o `WritePixelFast` para o estado dominante do 3D (alpha test GREATER + AFAIL FB_ONLY +
  blend + Z GEQUAL + fog): não feito; é o próximo alvo (os triângulos T8 são hoje o maior bloco).
- Paralelismo por bandas: fora do meu escopo (outro agente tem `gs_banded_backend.cpp`).

## Riscos
- O caminho SIMD do triângulo e o blend SIMD exigem AVX2/FMA/SSE4.1 (já exigidos pelo build).
- As tabelas separáveis/compactas são verificadas na inicialização; se um dia não forem válidas, os
  caminhos voltam ao genérico (FB CT32/CT24 inválido aborta, pois `frameCT32` também escolhe semântica).
- O padrão de FMA fixado reproduz o binário original **compilado com `/fp:fast /arch:AVX2`**. Se as
  flags mudarem, a referência também muda.
- `swizzledByteAddress`, `readTexelInline/Compact` e as tabelas duplicam a lógica de
  `PixelStorageTraits`; mudanças lá precisam ser espelhadas (o `compare`/`fuzz` detecta).

## Próximos passos
1. Triângulo T8 bilinear com alpha test + Z GEQUAL + blend + fog (≈ 40 % do tempo restante): caminho
   dedicado de escrita para esse estado; texel CLUT bilinear com os 4 índices numa leitura quando caem
   na mesma coluna de bloco.
2. Sprites minúsculos (18 mil/quadro, ~45 px): overhead fixo de ~2 500 ciclos por sprite (Submit,
   cópia de setup, colunas) — agrupar sprites consecutivos com o mesmo estado.
3. `LoadClutUnlocked` (~3 %): leitura inline do CLUT.

## Estado (04/11, 23:11)
Build final (todas as mudanças acima, exe `work/raster_exe`): `regress.ps1 -Scenario menu` IGUAL; `-Scenario scene3d` **6/6 IGUAL** (md5 a1b9c817, 7553e0cd, e669fdc7, d5c98af2, daf2f683, b10231d1). Gravações de teste em `work/gsrec/` (fora do git), baseline congelado do rasterizador em `work/raster_baseline_src/`.
