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
  consecutivas com a mesma chave viram um `Draw` (anel de vértices `MAP_NO_OVERWRITE`).
- **Blend**: `(A-B)*C>>7 + D` vira `Cs*fs + Cd*fd`. O shader pré-multiplica o termo de Cs (com As ou FIX) e o blender faz o
  termo de Cd com `SRC1_ALPHA` (dual source = As/128), `BLEND_FACTOR` (FIX/128) ou `DEST_ALPHA`; sinais negativos viram
  `SUBTRACT`/`REV_SUBTRACT`. `Cs*Ad` (0x58, comum neste jogo) usa `DEST_ALPHA` como fator de origem e o shader escala Cs por
  255/128 (o alvo guarda a/255). CT24: Ad = 0x80. Aproximados (contados em `approx_blend`): `Cd*(1+C)` e Ad do lado de Cd.
- **Alpha test**: `KEEP` = `discard`; `FB_ONLY`/`ZB_ONLY`/`RGB_ONLY` = dois passes no mesmo lote (aprovados com tudo;
  reprovados com máscara de cor/Z do AFAIL), como no PCSX2. `ATST=NEVER` vira só o ramo de falha.
- **Z**: D32F com `z·2⁻²⁴` (exato para Z16/Z24; Z32 com `2⁻³²`, com perda acima de 2²⁴); `ZTST` → `GREATER_EQUAL`/`GREATER`.
- **FBMSK** por byte vira máscara de canal; máscara de bits (shuffle CT16 com `0x7FFF`) vai para a CPU (80 draws/quadro).
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
`hwstats` mostra de onde vêm as texturas (alvo × memória); `hwprofile` amostra a thread do replay.

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

## 6. O que foi estudado do PCSX2 (regra 8)

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
2. Quebras de lote restantes (por troca de pipeline e de formato/tamanho de textura): mover TBW/PSM/tamanho para o vértice e
   ordenar as variantes de pipeline; o custo por quadro hoje é dominado pela thread do jogo (VU1), não pelo GS.
3. FBMSK de bits (shuffle CT16, 80 draws/quadro) e DATE/PABE ainda na CPU ou ignorados: implementar com cópia do alvo
   (shader blending) como no PCSX2.
4. Composição PMODE com dois circuitos na GPU (hoje: um circuito direto; com `PS2X_GS_HW_CPUFRAME` o host recebe o quadro da CPU).
5. Back-ends D3D12/Vulkan da `gs_rhi_hw.h` (seção 3) e substituição de texturas por hash (o caminho de textura passa por um ponto
   único — `FetchTexel`/página da cópia GPU — onde um cache por conteúdo pode ser inserido).
6. Rodar `explore2` e as outras regressões com `PS2X_GS=hw` antes de torná-lo padrão.
