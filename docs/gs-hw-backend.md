# Renderer de GS por hardware (`PS2X_GS=hw`) — 05/11/2026

Decisão do usuário (04/11, noite): abandonar o critério "byte-idêntico" (vale PSNR / equivalência visual) e fazer o GS **por
hardware de verdade**, no desenho do PCSX2 (GSdx/GSRendererHW): render targets reais, blend/Z do hardware, CLUT convertida,
render-to-texture, resolução interna escalável e uma RHI pronta para D3D12/Vulkan. O backend ROV (`PS2X_GS=d3d11`,
`gs-gpu-stage2.md`) continua como modo de precisão; nada dele foi alterado.

## 1. Arquivos

| Arquivo | Conteúdo |
|---|---|
| `ps2xRuntime/include/runtime/gs/gs_rhi_hw.h` | RHI gráfica nova (`gs_rhi::GfxDevice`): surfaces (RT/depth/sampled/storage), pipelines imutáveis, passes, constantes por draw, anel de vértices, compute, cópias, readback, apresentação com slot de filtro final |
| `ps2xRuntime/src/lib/gs/gs_rhi_hw_d3d11.cpp` | implementação D3D11 (janela própria ou headless) |
| `ps2xRuntime/include/runtime/gs/gs_hw_backend.h`, `src/lib/gs/gs_hw_backend.cpp` | `GSHwBackend` (coerência por página, cache de render targets, lotes, CLUT, fallback CPU, apresentação) |
| `ps2xRuntime/src/lib/gs/gs_hw_shaders.h` | HLSL: draw (VS/PS), write-back (RT → memória local na GPU), load (memória local → RT; Z por pixel shader com `SV_Depth`) |
| `gs_backend_factory.cpp`, `CMakeLists.txt` | `PS2X_GS=hw` e as duas fontes novas (`PS2X_HW_POSTFX=1` só em `gs_hw_backend.cpp`) |
| `ghost_pc/tools/gsreplay/hw_tools.cpp` (+ `gsreplay.h`, `build.cmd`) | harness headless: `hwcompare`, `hwbench`, `hwfirst`, `hwdraws`, `hwstats`, `hwprofile` |

Variáveis: `PS2X_GS=hw`; `PS2X_GS_SCALE=1..8` (resolução interna inteira, padrão 1; limitada pelo orçamento de VRAM);
`PS2X_GS_WINDOW=0` (janela escondida); `PS2X_GS_VSYNC=1`; `PS2X_GS_DEBUG=1` (camada de debug D3D11);
`PS2X_GS_FILTER=linear` (apresentação); `PS2X_GS_HW_CPUFRAME=N` (a cada N presents também devolve o quadro composto na CPU
para a janela do host e para os `shot`; 0 = nunca, só GPU); `PS2X_GS_HW_VERBOSE=1`; `PS2X_POSTFX_CRT=<preset>` aplica o filtro
CRT só na janela do renderer (nunca em `shot`/regressão).

## 2. Desenho

### 2.1 Três cópias da memória local e coerência por página (8 KiB)

- **CPU** (`m_vram`, o buffer do runtime): uploads host→local, CLUT, fallback, `shot` composto na CPU.
- **Cópia GPU da memória local** (buffer raw de 4 MiB, mesmo layout swizzled do `GSMem`): fonte de textura para qualquer PSM
  (T8/T4 com CLUT, CT16, alvos reinterpretados) — é o "cache de texturas por conteúdo" com invalidação por página: uma página
  só sobe quando a CPU a escreveu (`m_cpuNewer`).
- **Render targets**: uma textura RGBA8 por buffer de quadro e uma D32F por buffer de Z, chaveadas por (base, largura, PSM),
  no tamanho `largura×64 × 1024` linhas × `scale`. Cada alvo tem `span` (páginas das linhas usadas), `dirty` (páginas em que
  ele é a cópia mais nova) e `stale` (páginas em que a cópia GPU é mais nova que ele). `m_owner[página]` diz qual alvo tem
  dado sujo.

Movimentos (todos sob demanda):
| Quem precisa | O que acontece |
|---|---|
| draw num alvo S em páginas P | donos estrangeiros de P fazem write-back; páginas `stale` de S em P são carregadas da cópia GPU (compute / `SV_Depth`); após o draw S vira dono de P e os outros alvos que cobrem P ficam `stale` |
| textura com o mesmo layout de um alvo sujo (base, largura, CT32/CT24 ou CT16) | amostra o alvo direto (mantém a resolução interna); se o alvo é o próprio destino, cópia de feedback antes |
| textura com outro layout (T8 lendo um alvo CT32 etc.) | write-back do alvo na cópia GPU (compute, só as páginas sujas) e leitura pela cópia GPU com o swizzle do GSMem |
| CPU lê (local→host, CLUT de memória renderizada, fallback, `ReadVram`, `SnapshotVram`) | write-back + readback só das páginas necessárias (único stall de CPU) |
| CPU escreve (upload host→local, `ClearFramebuffer`, fallback) | páginas inteiras: o dono só perde a página; parciais: readback antes; depois `m_cpuNewer` e os alvos ficam `stale` |
| upload host→local em página **parcial e mais nova na GPU** (05/11) | sem readback: o dono faz write-back na GPU, os pixels caem em `m_vram` e só os blocos de 256 B escritos sobem para a cópia GPU (`patch_uploads`); a página continua "GPU mais nova" e `m_cpuValid[página]` marca os blocos em que a CPU está correta (o `LoadClut` usa isso para não fazer readback) |

Por que este modelo e não "alvo = verdade única" do PCSX2: este jogo lê o buffer principal CT32 como **T8 com largura 20**
(~9 mil sprites de pós-processo por quadro, ciclo 0x180↔0xA0) e faz *texture shuffle* em CT16 com FBMSK de bits. O PCSX2
trata isso com caminhos especiais (conversão de alvo para 8 bits, shuffle); aqui a cópia GPU da memória local resolve a
reinterpretação de forma geral, com um write-back por troca de dono, sem readback.

### 2.2 Draw

- O CPU repete o setup do `GSCpuBackend` (cobertura do sprite `[ceil(x0), ceil(x1))`, UV do sprite extrapolada às bordas do
  retângulo coberto, cor plana = v2/v1, Z como inteiro) e emite vértices `{x, y, z(u32), rgba, s, t, q, fog}` (32 bytes).
  Triângulos usam o rasterizador da GPU (centros de pixel em +0,5, como a CPU).
- **Sprites em tiras** (`x1 = x0 + 63,5`): como as bordas viram inteiros de 1x antes da escala, a tira cobre exatamente os
  mesmos pixels de 1x em qualquer `scale` (sem fendas).
- **Estado por lote**: (alvo, Z, fonte de textura, pipeline, pipeline do 2º passe, scissor, FIX, constantes). Primitivas
  consecutivas com a mesma chave viram um `Draw` (anel de vértices `MAP_NO_OVERWRITE`). Desde a otimização de 05/11 (seção
  5.3) o estado de textura/TFX/TEXA/teste de alfa/flags vai **por primitiva no vértice** (`HwVertex::st`, 4 palavras
  empacotadas; o shader reconstrói `gTex0/gTex1/gMisc`), então troca de textura (TBW/PSM/tamanho/clamp) não quebra mais o lote;
  as constantes do lote ficam só com o que é por alvo/fonte/pipeline (xform, FIX/blend, cor do fog, `src`, passe do AFAIL).
- **Linhas e pontos** viram quads de 1 pixel sobre os mesmos pixels inteiros do Bresenham da CPU (sem textura, como a CPU).
- **Blend**: `(A-B)*C>>7 + D` vira `Cs*fs + Cd*fd`. O shader pré-multiplica o termo de Cs (com As ou FIX) e o blender faz o
  termo de Cd com `SRC1_ALPHA` (dual source = As/128), `BLEND_FACTOR` (FIX/128) ou `DEST_ALPHA`; sinais negativos viram
  `SUBTRACT`/`REV_SUBTRACT`. `Cs*Ad` (0x58, comum neste jogo) usa `DEST_ALPHA` como fator de origem e o shader escala Cs por
  255/128 (o alvo guarda a/255). CT24: Ad = 0x80. Aproximados (contados em `approx_blend`): `Cd*(1+C)` e Ad do lado de Cd.
- **Alpha test**: `KEEP` = `discard`; `FB_ONLY`/`ZB_ONLY`/`RGB_ONLY` = dois passes no mesmo lote (aprovados com tudo;
  reprovados com máscara de cor/Z do AFAIL), como no PCSX2. `ATST=NEVER` vira só o ramo de falha.
- **Z**: D32F com `z·2⁻²⁴` (exato para Z16/Z24; Z32 com `2⁻³²`, com perda acima de 2²⁴); `ZTST` → `GREATER_EQUAL`/`GREATER`.
- **FBMSK** por byte vira máscara de canal; máscara de bits (shuffle CT16 com `0x7FFF`, ~27 sprites/present) **sem blend**
  é feita no shader: cópia do alvo na caixa da primitiva (superfície de feedback), lida em `SV_Position`, e
  `(src & ~m) | (dst & m)` (flag `F_DSTMASK`, máscara em `gSrc.y`). Com blend, ou com a textura vindo de outro alvo (t0
  ocupado), continua na CPU (contado em `cpu_why bitmask`).
- Não emulados ainda (contados): DATE, PABE, COLCLAMP=0 (wrap), dither.

### 2.3 Resolução interna escalável

O fator inteiro `scale` existe desde a criação dos alvos (tamanho `×scale`, viewport e scissor `×scale`); write-back lê a
amostra superior esquerda de cada bloco `scale×scale`, load replica o texel. Amostrar um alvo como textura usa a grade de 1x
(pendente: amostrar na grade escalada para não perder detalhe em render-to-texture). Efeitos de tela cheia em pixels do guest
continuam corretos porque todas as coordenadas do guest são de 1x e só o rasterizador vê a escala. Orçamento: ~24 alvos de
640×1024×4 B × scale² ≤ metade da VRAM dedicada (DXGI); se não couber, o `scale` é reduzido (log `[gs-hw]`).

### 2.4 Apresentação

`Present` escolhe o buffer como a CPU no caso comum (circuito 1/2, `preferredSource`), carrega páginas `stale` dele e faz o
blit do alvo **direto da GPU** para a janela (sem readback). O quadro devolvido ao host é um 64×64 preto (a janela do host fica
viva) salvo com `PS2X_GS_HW_CPUFRAME=N`. Slot de passe final: `GfxDevice::SetPresentFilter` (HLSL com
`PresentFilterMain(PresentFilterIn)`), usado pelo filtro CRT (`shaders/gs_postfx_crt.hlsl`, `ps2_postfx::computeCrtParams`).

## 3. Interface RHI para D3D12/Vulkan

`gs_rhi_hw.h` foi desenhada para mapear 1:1:

| RHI | D3D11 (feito) | D3D12 | Vulkan |
|---|---|---|---|
| `Surface` (RT/depth/sampled/storage) | `ID3D11Texture2D` + views | recurso + descritores RTV/DSV/SRV/UAV; estado rastreado por surface | `VkImage` + views; layout rastreado |
| `Pipeline` (programa + blend + depth + máscara; formatos fixos RGBA8/D32F) | blend/depth state objects | PSO (root signature fixa: CBV b0, tabela t0..t3, UAV u0..u1, sampler estático s0/s1) | `VkPipeline` (dynamic rendering, descriptor set fixo) |
| `BeginPass/EndPass` | `OMSetRenderTargets` | `OMSetRenderTargets` + barreiras | `vkCmdBeginRendering` + barreiras de layout |
| `SetConstants` (≤4 KiB) | anel de cbuffer com offsets (D3D11.1) | CBV raiz no anel de upload | UBO dinâmico com offset |
| `Draw(vertices)` | anel `MAP_NO_OVERWRITE` | anel em heap de upload + VBV | anel host-visible |
| `Dispatch` | CS | CS + barreiras UAV | compute + barreiras |
| `ReadBuffer/ReadbackSurface` (bloqueiam) | staging + `Map` | readback heap + fence | buffer host-visible + fence |
| `PresentSurface` + filtro | swapchain flip | swapchain + passe final | swapchain + passe final |

Diferenças a cuidar ao portar: (1) transições implícitas — D3D11 as faz sozinho; D3D12/Vulkan precisam rastrear o último uso
de cada surface/buffer (RT, depth, sampled, storage, copy) e emitir barreiras no `BeginPass`/`Dispatch`/cópia; (2) anéis
(vértices/constantes) precisam de fences por quadro para não sobrescrever o que a GPU ainda lê; (3) dual-source blend existe
nos três (Vulkan: `dualSrcBlend`); (4) `SV_Depth` e RWTexture2D `unorm float4` têm equivalentes diretos (Vulkan precisa de
`shaderStorageImageWriteWithoutFormat` ou formato declarado); (5) shaders: HLSL via DXC (`-spirv` no Vulkan).

## 4. Verificação

Harness headless (`tools/gsreplay`, `build.cmd ... hwonly`), gravações `work/gsrec/scene3d_*.rec` (dados do jogo, fora do git):
`gsreplay hwcompare <rec> <scale> <cada N presents> [dir]` roda CPU e HW em lockstep e, a cada N presents, sincroniza o HW
(write-back + readback) e compara cada buffer de quadro desenhado (PSNR de RGB).

Outras ferramentas do harness: `hwfirst <rec> <scale> <passo> <tol> <bytes> [início]` para no primeiro registro que piora as
páginas do próprio alvo (imprime a primitiva e os pixels); `hwdraws <rec> <fbp>` lista estados distintos que desenham num alvo;
`hwstats` mostra de onde vêm as texturas (alvo × memória); `hwprofile` amostra a thread do replay (amostragem de 1 ms com
`timeBeginPeriod`; antes era ~15 ms e dava poucas amostras); `hwuploadcheck <rec>` passa só as transferências pelo
`GSCpuBackend` e pelo upload rápido do `GSHwBackend` e compara a memória local byte a byte; `hwdraws <rec> 0xFFFF` lista os
draws com FBMSK de bits. A linha de estatísticas ganhou `cpu_why=(prim fpsm zpsm tpsm bitmask)`, `upload_mb`,
`patch_uploads`, `clut_no_readback`, `clut_loads`, `clut_skipped`. `PS2X_GS_HW_SLOWUPLOAD=1` volta ao upload do
`GSCpuBackend` (o upload rápido também se desliga sozinho com `PS2X_GS_RECORD`, porque o gravador mora no backend de referência).

No jogo: `tools/regress.ps1`-like com `PS2X_GS=hw PS2X_GS_HW_CPUFRAME=1` (o `shot` sai do quadro composto na CPU depois de
sincronizar tudo) e `../tools/image_regress.py logs/final/shots_baseline_cpu <shots> --min-psnr 30`.

## 5. Resultados

Máquina: RTX 2060 (6 GiB), outros agentes rodando ao mesmo tempo (números absolutos com carga).

### 5.1 Harness (replay das gravações, headless)

| Gravação | Comparação (último present) | PSNR display (0x000, CT24) | PSNR buffer principal (0xA0) |
|---|---|---|---|
| `scene3d_7570` (3 presents, 84 mil registros) | x1 | 37,0 dB | 36,2 dB |
| `scene3d_7501` (64 presents, 819 mil registros) | x1, present 32 / 64 | 46,4 / 36,2 dB | 45,3 / 35,4 dB |
| `scene3d_7628_new` (com HUD, gravada com o exe atual) | x1 | 35,8 dB | 35,2 dB |
| `scene3d_7570` | x2 / x4 (comparado em 1x pelo write-back) | 32,0 / 30,9 dB | 29,4 / — dB |

Diferença visível restante: o **brilho do holofote** (cadeia de glow 0x172 → 0x180 → 0x188…0x1AE, sprites de 64×64 que se
realimentam: o mesmo alvo é textura e destino com deslocamento de meio texel e blend aditivo). A CPU de referência lê, dentro do
mesmo sprite, texels que ela própria acabou de escrever (efeito do cache de página de textura do `GSCpuBackend`); o hardware lê a
cópia de feedback do início da primitiva, então o glow sai mais fraco. Não é óbvio qual dos dois é o comportamento do GS real
(o GS também tem cache de textura); fica como pendência. O resto (cena, personagem, HUD) bate com ±1–3 nos canais.

Tempo do GS no replay (`hwbench`, inclui esperar a GPU no fim) contra o rasterizador de CPU atual (`bench new`), mesma gravação:
| Gravação | CPU (`GSCpuBackend`) | HW x1 | HW x4 |
|---|---|---|---|
| `scene3d_7570` | 588 ms | 108–119 ms | 152 ms |
| `scene3d_7501` (64 presents) | 4 311 ms | 1 208 ms | — |

### 5.2 No jogo (cena 3D, janela de vsync 7530→7640, `guest_vsync_hz` integrado do `PS2X_FPS_LOG`)

Exe: `build_perf` de 05/11 01:36 (inclui também as mudanças dos outros agentes da noite, ex. VU1), copiado para `work/hw_exe4`;
pad `regress_scene3d.txt`, `PS2X_GS_WINDOW=0`, `PS2X_GS_HW_CPUFRAME=0` (apresentação só na GPU); máquina sem outro
`ps2EntryRunner` rodando. Taxa = 110 vsyncs / tempo entre os instantes em que a soma de `guest_vsync_hz × dt` cruza 7530 e 7640
(o CSV tem uma linha por segundo: resolução de ~±10%). Script: `logs/hw/` + `hw_measure.sh` (scratchpad da sessão).

| Modo | vsync/s na janela 7530→7640 | Referências (mesma métrica, exes antigos de `logs/final/`) |
|---|---|---|
| `PS2X_GS=hw`, scale 1 | **12,2 Hz** | baseline CPU 3,0; CPU 6,1; d3d11-ROV 6,9; cpu-thread 12,1 |
| `PS2X_GS=hw`, scale 4 (2560×2048 interno) | **11,0 Hz** | — |

Correção no jogo (`PS2X_GS_HW_CPUFRAME=1`, `shot` contra `logs/final/shots_baseline_cpu`): q07300 71,7 dB; q07500 73,6 dB;
q07564 40,8 dB; q07628 37,0 dB; q07692 37,0 dB; q07700 36,9 dB — todos acima de 30 dB; diferença visível só no glow do holofote.

Leitura: com o GS por hardware a cena 3D sobe para ~12 vsync/s e a escala 4× custa pouco (~10%): o GS deixou de ser o gargalo; o
quadro agora é limitado pela thread do jogo (VU1/EE/IOP), como já indicava o `cpu-thread` (~12 Hz). Para 50 Hz o próximo passo
é o VU1 (outro agente) e tirar o GS da thread do jogo (preparo de primitivas numa thread própria, com a fila do `cpu-thread`).

### 5.3 Otimização do custo de CPU/driver (05/11, 02:10–03:40)

Ponto de partida: o perfil medido no jogo (`gameplay-profile-hw-2026-11-05.md`): espelho CPU do GS 14 % da thread do jogo
(`UploadImage`/`WriteP8`/`DrawSpriteTextured`), readbacks ~8 ms/quadro, ~1 000 draws e ~720 quebras de lote por quadro. O
replay (`hwbench`/`hwprofile`, headless, inclui esperar a GPU no fim) reproduz isso: no `scene3d_7501` (64 presents) o tempo
ia para submits 630 ms (com 11 % em `DrawSpriteTextured` de fallback), uploads 335 ms, `LoadClut` 152 ms e 22 % das amostras
em `NtWaitForSingleObject` (219 readbacks). Logs: `logs/hwopt/` (fora do git).

Passos, em ordem, com o `hwbench scene3d_7501 1 N` (melhor de N; ms por present gravado):

| # | Mudança | ms/present | O que mudou nos contadores |
|---|---|---|---|
| 0 | base (exe de 01:36) | **17,60** | api_draws 41 529, flush_key 28 488 (tex 14 384, pso 10 602, alvo 3 324), cpu_prims 3 460, readbacks 219 (226 ms) |
| 1 | upload host→local direto em `m_vram` (traits do GSMem inline, sem chamada virtual/divisão/lock por pixel) + fallback com páginas da caixa da primitiva | 12,73 | uploads 335 → 195 ms |
| 2 | linhas/pontos na GPU | 13,43 (ruído) | as 2 700 linhas da gravação estavam fora do scissor (agora `tri_out`), ganho nulo aqui |
| 3 | FBMSK de bits sem blend na GPU (cópia do alvo + merge no shader) | 12,70 | cpu_prims 0; `DrawSpriteTextured` some do perfil; mas surgem 361 readbacks de 1 página (uploads parciais em páginas GPU) |
| 4 | upload parcial em página GPU sem readback (patch de blocos de 256 B) | 11,90 | readbacks 361 → 181 (sobravam no `LoadClut`) |
| 5 | `LoadClut` sem readback quando os blocos da CLUT vieram de um patch (`m_cpuValid`) | 10,72 | readbacks 181 → **18**, readback 226 → 47 ms |
| 6 | estado de textura/TFX/alfa por primitiva no vértice | 11,3 (ruído*) | api_draws 41 529 → **25 556**, flush_key 28 488 → 14 158 (tex 0), vtx 135 → 241 MB |
| 7 | filtros de estado redundante na RHI (SRVs, constantes, scissor) + leituras da CLUT inline | 11,7 (ruído*) | — |
| 8 | estado empacotado (vértice 64 → 52 B) | 11,27* | vtx 241 → 195 MB |
| 9 | `LoadClut` idêntico ao anterior, de páginas não escritas desde então (versão por página), é pulado | 9,12 | 78 % das cargas de CLUT puladas (39 722 de 50 871) |
| 10 | upload com linha da tabela de swizzle em cache e store direto | **6,69** | uploads 195 → ~100 ms; `hwuploadcheck`: memória **byte-idêntica** à do `GSCpuBackend::UploadImage` nas 3 gravações |
| 11 | `UpdateBuffer` (slot de CLUT, páginas, patch) sem encerrar o passe | 8,4–9,1* | passes 19 764 → 11 399 |

(*) medidos com `ps2EntryRunner` de outro agente rodando ao mesmo tempo (o replay varia ±30 % com essa carga); o ganho do
6 aparece no `scene3d_7628_new` (3 presents): 123,8 → 88,9 ms; no fim (passo 11) **60,3 ms** (2,05×).

Total no replay: **17,60 → 6,7–9,1 ms/present (2–2,6×, conforme a carga da máquina)**; readbacks 219 → 18; draws −38 %;
passes −52 %; fallback CPU 3 460 → 0.
Qualidade (`hwcompare`, último present; igual ao de antes nas casas mostradas): `scene3d_7570` display 37,02 dB / buffer
principal 36,17 dB; `scene3d_7628_new` 35,80 / 35,17 dB. (A visão CT16 de 0xA0 — os mesmos bytes lidos como 16 bits, o
alvo do shuffle — dá 18,8 dB tanto antes como depois: amplifica as mesmas diferenças de ±1–3 dos canais.)

No jogo (cena 3D, janela de vsync 7530→7640, mesma métrica da 5.2, `PS2X_GS_WINDOW=0`, `PS2X_GS_HW_CPUFRAME=0`,
`PS2X_TARGET_FPS=0`; exe `work/hw_exe_opt` de 05/11 02:31; script `hwopt_measure.sh` no scratchpad da sessão):

| Exe | vsync/s | Observação |
|---|---|---|
| base `work/hw_exe4` (01:36) | **12,22** | 02:35; um `ps2EntryRunner` de outro agente rodando junto (bate com os 12,2 da 5.2) |
| `work/hw_exe_opt` (passos 1–10, 02:31) | **17,25** (+41 %) | 02:32; outro `ps2EntryRunner` entrou aos 30 s |
| `work/hw_exe_opt2` (passos 1–11, 02:42) | **18,76** (+54 %) | 02:42; sem outro jogo rodando no início |

Contadores no jogo, intervalo de presents 7200→7500 (o mesmo do perfil; quase todo gameplay): antes / depois (exe opt2):
api_draws +148 667 / **+36 811**; flush_key +104 545 / +20 234; `gpu_readback_ms` **+1 193 ms / +0 ms** (zero readbacks no
gameplay); cpu_prims +2 895 / **0**; passes +16 458 no intervalo (antes: ~1 por draw nas cenas de CLUT trocada a cada sprite).

Regressão (`PS2X_GS_HW_CPUFRAME=1`, exe opt; `shot` contra `logs/final/shots_baseline_cpu`, `image_regress.py --min-psnr 30`,
log `logs/hwopt/regress_opt_*`): q07300 71,70 dB; q07500 73,56; q07564 40,79; q07628 36,98; q07692 36,48; q07700 36,92 —
todos > 30 dB (antes: 71,7 / 73,6 / 40,8 / 37,0 / 37,0 / 36,9; q07692 −0,5 dB, dentro da tolerância). Com `CPUFRAME=1` a
janela deu 17,72 vsync/s. (O `image_regress` acusa "FALHA" porque os limiares de erro absoluto ficam em 0 por padrão; o
critério combinado é o PSNR.)

O que **não** foi feito (decisão, com o porquê):
- Swizzle de upload em compute shader: o upload na CPU ficou ~2,4× mais rápido e exato, e manter `m_vram` correto evita
  readbacks no `LoadClut`/fallback. Mover para a GPU exigiria CLUT na GPU (a CLUT do fallback de referência mora na CPU).
- Array de texturas / atlas: as quebras por textura eram de *parâmetros* (TBW/PSM/tamanho/clamp), não de recurso — a textura
  vem da cópia GPU da memória local (um buffer só). Levar os parâmetros ao vértice zerou essas quebras sem atlas.
- Contexto diferido / thread de submissão: com 18 readbacks e −38 % de draws o driver deixou de dominar o replay; o perfil
  restante é submit (setup de primitiva) e upload. Fica para depois de medir o jogo de novo.
- Laço da janela com `PS2X_TARGET_FPS=0` (gira a ~5 100 quadros/s): o laço é do `ps2_runtime.cpp` (fora dos arquivos deste
  backend). Com o padrão (`PS2X_TARGET_FPS` ausente = 60) ele já dorme; nas medições de benchmark ele continua roubando ~60 %
  de um núcleo. Pendência registrada na seção 8.

## 6. O que foi estudado do PCSX2 (regra 8)

Rodada de otimização (5.3): nenhuma consulta à rede; as ideias (estado por primitiva para não quebrar lotes, FBMSK de bits
com cópia do alvo, evitar readback) são as do desenho conhecido do `GSRendererHW`/`GSTextureCache`, sem código copiado.

PCSX2 (GPL-3.0, mesma licença do PS2Recomp). Estudo **de memória** (conhecimento prévio do desenho de `pcsx2/GS/Renderers/HW`:
`GSRendererHW`, `GSTextureCache` com fontes/alvos por (TBP, TBW, PSM), `GSDevice11` e `tfx.fx`/`tfx.hlsl`), sem acesso à rede
nesta sessão e **sem copiar código**. Ideias aproveitadas: alvos por (base, largura, PSM) com escala interna; blend do GS
mapeado no blender com dual-source e o resto no shader; AFAIL em dois passes; cópia do alvo para feedback; Z em D32F.
Divergência consciente: em vez de converter alvos entre formatos (shuffle/8 bits), a reinterpretação passa por uma cópia da
memória local na GPU (write-back por compute), herdada do backend ROV.

## 7. Bugs encontrados no caminho (para não repetir)

1. **Ordem lote × atualização da memória GPU**: um upload host→local (fonte de HUD/glifos reenviados entre draws) ou um
   write-back chegava à cópia GPU **antes** de o lote pendente, que ainda lia o conteúdo antigo, ser desenhado → ícones e letras
   trocados no HUD. Agora `UploadPagesLocked` e `WritebackLocked` desenham o lote pendente antes.
2. **Z inteiro**: o GS guarda e compara Z inteiro; D32F guardava o valor interpolado com fração e passes coplanares com
   `ZTST=GEQUAL` (máscara de alfa do glow) falhavam. O pixel shader arredonda (`SV_Depth = floor(z + 0,5)`), como a CPU.
3. **Rastreio de headers do ninja**: mudar `gs_hw_backend.h` não recompilou `gs_backend_factory.cpp` → `make_unique` com o
   tamanho antigo → corrupção de heap (`0xC0000374`) e, antes disso, estatísticas absurdas (3 alvos em vez de 23). Ao mudar o
   header: `touch` em `gs_backend_factory.cpp`, `gs_hw_backend.cpp` e `gs_rhi_hw_d3d11.cpp` antes do `tools/build.ps1`.
4. Aquecimento do harness: o device (e a compilação de shaders, ~0,5 s) nascia no primeiro draw cronometrado.
5. `Cs*Ad` (0x58) aproximado com fator 1 deixava o personagem claro demais; resolvido com `DEST_ALPHA` × 255/128.

## 8. Pendências (por ganho esperado)

1. **Glow do holofote** (feedback dentro do mesmo sprite, ver 5.1): decidir a semântica (GS real) e, se for a da CPU, desenhar os
   sprites auto-referentes em tiras/colunas com cópia entre elas.
2. Quebras de lote restantes: por pipeline (~10,6 mil em 64 presents: blend/Z/máscara mudam entre primitivas vizinhas) e por
   alvo (3,3 mil). Opções: blend no shader com cópia do alvo só quando barato, ou ordenar variantes respeitando a ordem de
   pintura (exige provar que não há sobreposição). Por textura: resolvido (5.3).
3. FBMSK de bits **com blend** e com textura vinda de outro alvo, DATE e PABE: ainda CPU/ignorados (FBMSK sem blend: GPU, 5.3).
4. Laço da janela (`ps2_runtime.cpp`) com `PS2X_TARGET_FPS=0`: reapresentar só quando houver quadro novo (esperar evento) sem
   mudar o ritmo do guest nem o relógio do pad; não é arquivo deste backend.
5. Upload: ainda ~1,9 ns/byte na CPU (P8 domina). Próximo degrau: swizzle por bloco de 256 B (cópia de colunas, como o
   GSLocalMemory do PCSX2) ou upload linear + compute com CLUT na GPU.
6. `Present` da janela e contenção do `m_mutex` (recursivo) com a thread da janela: não medido nesta rodada.
7. Composição PMODE com dois circuitos na GPU (hoje: um circuito direto; com `PS2X_GS_HW_CPUFRAME` o host recebe o quadro da CPU).
8. Back-ends D3D12/Vulkan da `gs_rhi_hw.h` (seção 3) e substituição de texturas por hash (o caminho de textura passa por um ponto
   único — `FetchTexel`/página da cópia GPU — onde um cache por conteúdo pode ser inserido).
9. Rodar `explore2` e as outras regressões com `PS2X_GS=hw` antes de torná-lo padrão.
