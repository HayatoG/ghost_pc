# Backend de GS por GPU (D3D11), etapa 2 — implementação (04/11/2026)

Revisa `../../docs/gs-gpu-step2-design.md` à luz do perfil de gameplay (`gameplay-profile-2026-11-04.md`)
e descreve o que foi implementado. Seletor: `PS2X_GS=d3d11` (o padrão continua `cpu`).

## 1. Reavaliação do design contra o perfil

O design previa **alvos lineares na GPU** (um cache de render targets por `(FBP, FBW, PSM)`) com
coerência por página e as fases E2 sprite → E3 triângulo → E4 textura direta → E5 CLUT → E6
render-to-texture. O perfil da cena 3D muda a conta:

| Medido na gameplay | Consequência para o design de alvos lineares |
|---|---|
| 98% dos draws texturizados, 90% T8 (CLUT) | E2/E3 sozinhas quase não tiram nada da CPU |
| render-to-texture 71,6% dos draws / 37,6% dos px | E6 é obrigatória desde o início; com alvos lineares cada leitura de alvo como textura exige casar PSM/largura ou fazer write-back + conversão |
| ~9 000 sprites de 45 px num alvo auxiliar em ciclos de 192+3 draws | o custo por draw domina: uma chamada de API por draw ou por troca de estado não serve |
| blend "outro"/FBMSK/AFAIL/DATE presentes | blend fixo do D3D11 cobre a maioria, mas não tudo: sobra fallback e pingue-pongue |

**Decisão: trocar o modelo de alvos lineares por "VRAM inteira na GPU".** A GPU guarda uma cópia
**byte a byte** dos 4 MiB da memória local, no mesmo layout swizzled da CPU (`GSMem`). O pixel shader
calcula o endereço de cada pixel de frame, Z e textura com as mesmas tabelas da CPU. Assim:

- render-to-texture, CLUT, reinterpretação de PSM (alvo CT32 lido como T8 etc.) funcionam **sem cache
  de alvos** e sem conversão: é a mesma memória;
- toda a aritmética do GS fica no shader, em inteiros, copiada do `GSCpuBackend` (blend com qualquer
  A/B/C/D e fator > 0x80, FBMSK por bit, AFAIL, DATE, Z16S comparando 32 bits, CT24, CT16, fog,
  TFX, TEXA, wrap/region, bilinear) — o fallback de CPU fica só para linhas/pontos (0% medido);
- a coerência CPU↔GPU vira simples: por página de 8 KiB, ou a CPU ou a GPU tem a versão mais nova.

Custo da escolha: sem blend/Z de hardware. A ordem entre primitivas que se sobrepõem é garantida por
**ROV** (*rasterizer-ordered view*, D3D11.3; "fragment shader interlock" no Vulkan): o shader lê uma
textura ROV no início e escreve no fim, e o hardware serializa as invocações do mesmo pixel na ordem
das primitivas. Isso permite **um único draw para milhares de primitivas com estados diferentes**
(o estado vai por primitiva num structured buffer). Escritas de 16 bits (Z16S, CT16) usam atômicos,
porque dois pixels diferentes dividem a mesma palavra de 32 bits. Projetos que seguem a mesma ideia
(VRAM emulada na GPU, ordem por interlock/compute): paraLLEl-GS (Vulkan compute), que serviu só de
referência conceitual; nenhum código usado.

Alternativas descartadas: alvos lineares (motivos acima); um draw por primitiva com barreira (20 mil
draws por quadro); compute com binning por tile (melhor no longo prazo, muito maior para fazer agora).

## 2. Como funciona

Arquivos: `ps2xRuntime/include/runtime/gs/gs_gpu_backend.h`, `src/lib/gs/gs_gpu_backend.cpp`,
`src/lib/gs/gs_gpu_shaders.h` (HLSL embutido), `include/runtime/gs/gs_rhi.h` + `src/lib/gs/gs_rhi_d3d11.cpp`
(buffers raw/structured, cópia, readback por faixas, programas, draw procedural sem RT, dispatch,
capacidade ROV), `gs_backend_factory.cpp` (opções).

- **Recursos GPU**: `vram` (4 MiB, UAV, viva), `tex` (4 MiB, cópia da `vram` no início de cada lote,
  usada para ler texturas), anel dinâmico de primitivas (32 768 × 208 bytes), anel de CLUT (4096
  slots de 512×u16), tabelas de endereço dentro da página (8 famílias, 45 056 u16) e a textura ROV
  2048×2048.
- **Tabelas de swizzle**: geradas em tempo de execução **sondando as funções de leitura da CPU**
  (`GSMem::ReadCT32`, `ReadP4`…) sobre um buffer com o índice de cada pixel; nada é transcrito à mão.
  Na criação, um **autoteste** lê 3 328 pixels (13 PSMs × 4 bases × 4 larguras × 16 pontos) de um
  padrão pseudoaleatório pelo HLSL e compara com `GSMem` na CPU; se uma divergir, a GPU não desenha.
- **Primitiva**: o CPU repete o setup do `DrawSprite`/`DrawTriangle` (cantos com `ceil`, bbox recortada
  no scissor, `denom`, UV do sprite) e grava um `GpuPrim`. O vertex shader gera o quad da bbox a partir
  de `SV_VertexID`; o pixel shader faz o teste de aresta da CPU (centro do pixel, tolerância 1e-4 nas
  três arestas), interpola como a CPU e executa `WritePixelFast`.
- **Coerência por página** (512 páginas): `gpuDirty` / `cpuDirty`. Antes de um acesso da CPU
  (upload host→local, local→local/host, `LoadClut`, draw de fallback, `ClearFramebuffer`,
  `ReadVram/WriteVram`, `SnapshotVram`, `Present`) as páginas tocadas voltam da GPU (readback por
  faixas); páginas que um upload cobre inteiras são descartadas sem readback. Antes de um draw na
  GPU as páginas `cpuDirty` que ele toca sobem (`UpdateSubresource`).
- **Lote**: primitivas se acumulam e viram um `Draw`. O lote é fechado quando (a) a textura da nova
  primitiva foi escrita no lote (a leitura vem da cópia `tex`), (b) a mesma página é escrita com outro
  layout (frame × Z, outra largura/PSM: a ROV ordena por posição de pixel, não por endereço), (c) a CPU
  precisa das páginas, (d) anel cheio, (e) `Flush()` (FINISH) ou `Present`.
- **CLUT**: espelho do buffer de CLUT da CPU com a mesma regra de CLD; cada CLUT carregada vira um
  slot novo no anel; a primitiva guarda o índice do slot (sem fechar o lote por troca de paleta).
- **Present**: lote fechado, todas as páginas `gpuDirty` voltam para a VRAM da CPU, cópia privada de
  4 MiB e composição pelo `GSCpuBackend` (mesmo PMODE/DISPFB/heurísticas da referência). O
  `PresentationFrame` (e os `shot`) saem desse caminho; a janela D3D11 mostra o mesmo quadro.
  Composição na GPU (sem readback) é a etapa E8, não feita.
- **Threads**: um mutex no backend; o device nasce no primeiro `Present` (thread da janela, dona da
  janela D3D11) e é usado pela thread do jogo sob o mesmo mutex.

Variáveis: `PS2X_GS=d3d11`; `PS2X_GS_GPU_DRAW=0` volta à etapa 1 (CPU desenha, GPU só apresenta);
`PS2X_GS_WINDOW=0` esconde a janela D3D11; `PS2X_GS_DEBUG=1` camada de debug; `PS2X_GS_VSYNC=1`.
Log a cada 10 s: `[gs-gpu] presents= gpu_prims= cpu_prims= skipped= batches= (tex= alias= cpu= ring=
clut= flush= present=) uploads_pages= readbacks= readback_pages= readback_ms= present_sync_ms= ...`.

## 3. Diferenças conhecidas em relação à referência de CPU

- **Ordem/arredondamento de float**: a CPU compila com `/fp:fast` (pode fundir multiplicações e
  somas); o HLSL usa `precise`. Diferenças só em pixels de borda de triângulo e no arredondamento
  de UV/cor, de ±1.
- **Z de triângulo em float** (a CPU interpola em `double`): exato para Z16S (≤ 65 535); Z24/Z32
  grandes podem diferir em ±1.
- **Cache de páginas de textura da CPU** (`TexturePageCache`): a CPU "congela" uma página de textura
  até troca de página/TEXFLUSH; a GPU lê a cópia do início do lote. Só difere se o jogo desenhar
  numa textura e lê-la na mesma primitiva/lote sem TEXFLUSH.
- `LoadClut` do espelho lê a memória direto (sem o cache de páginas da CPU).
- Requer ROV (NVIDIA Maxwell 2+, AMD Vega+, Intel Gen9+) e `MapNoOverwriteOnDynamicBufferSRV`;
  sem isso a GPU não desenha e tudo fica na CPU (etapa 1).

## 4. Verificação

Ferramenta: `tools/regress.ps1 -Renderer d3d11 -Exe work\gsgpu_exe\ps2EntryRunner.exe` (referência = capturas do
baseline de CPU em `work/reference_regress/`), com `PS2X_GS_WINDOW=0`. Máquina carregada por outros agentes:
nenhum número de FPS absoluto aqui.

| Cenário / modo | Resultado |
|---|---|
| Autoteste de endereçamento (13 PSMs) | 3 328/3 328 iguais à CPU |
| `menu`, `PS2X_GS_GPU_DRAW=0` (etapa 1) | idêntico (MD5) |
| `menu`, GPU desenhando tudo | 2 154 px diferentes, erro máx. 2, PSNR 72,4 dB |
| `scene3d`, 6 capturas, GPU desenhando tudo | q07500 idêntico; gameplay (q07564…q07700): erro máx. 8–9, ~130–150 mil px com ±1–3, PSNR 51–55 dB; q07300 ver abaixo |
| `PS2X_GS_GPU_VERIFY=37` (1 em 37 primitivas desenhada também na CPU e comparada) | 77 524 conferidas; 15 673 com alguma diferença, 40 acima de ±2 — todas triângulos com textura bilinear e STQ (perspectiva) ou Gouraud com blend; ±1–4 por canal; poucos texels de borda (4–26 palavras) com erro grande por vizinho bilinear diferente |

Leitura: o erro restante é arredondamento de float (UV em perspectiva, interpolação de cor) que se espalha pelas
passadas de pós-processamento de tela cheia; nenhuma geometria a mais ou a menos.

**q07300** (cena de corte com efeito de rastro): sai ou **byte-idêntica** (MD5 a1b9c817) ou com um MD5 fixo
alternativo (4632cdca, PSNR 42,6 dB), conforme a rodada, com o `shot` no mesmo vsync do guest. É sensível ao ponto
em que o host apresenta (o quadro usa o anterior como textura); tratado como temporização, não como erro de desenho.

### Bugs encontrados e corrigidos no caminho (para não repetir)

1. **`SV_VertexID` não inclui `StartVertexLocation`** no D3D11: desenhar a partir do meio do anel de primitivas lia
   primitivas erradas (menu todo errado). Agora cada lote escreve no início de um buffer descartado e desenha do 0.
2. **Cache de página de textura da CPU**: o `GSCpuBackend` guarda 1 página de textura entre primitivas; depois de um
   readback a memória muda "por trás" dele. O backend chama `TextureFlush()` da referência após cada readback.
3. **Ordem dentro do lote**: uma textura ROV separada como "trava" deixava o compilador/driver adiantar leituras do
   buffer comum para fora da seção ordenada → oclusão errada (uma cunha escura no cenário). Correção: o próprio buffer
   da VRAM é `RasterizerOrderedByteAddressBuffer` (atômicos funcionam nele para as metades de 16 bits).
4. **Dependência de headers no ninja**: com o log de build perdido, mudar `gs_gpu_backend.h` não recompilava a fábrica
   → tamanho de objeto diferente entre unidades (corrupção). Ao compilar, tocar os 3 `.cpp` que incluem o header.
5. Z em `double` (como a CPU) e FMA explícito foram testados e **pioraram** a comparação; ficam atrás de
   `PS2X_GS_GPU_DOUBLE_Z=1` / `PS2X_GS_GPU_FMA=1|2|3` (padrão desligado).

6. **Limite de textura em REGION_REPEAT**: as passadas de pós-processo usam `REGION_REPEAT` (UMSK=0x3F7, VMSK=0x3F9);
   o limite conservador `msk|fix` (≈1015) fazia cada sprite "ler" a textura inteira, que cobre o alvo auxiliar
   (fbp 0x180) → ~3 000 fechamentos de lote por quadro. Agora o intervalo de UV da primitiva passa pela regra de wrap
   (REGION_REPEAT: `[fix, (2^bits(max) − 1) | fix]`; REGION_CLAMP: recorte; REPEAT só se não der a volta). Mesma
   imagem, lotes por quadro ~3 000 → ~216.

## 4b. Desempenho (A/B relativo, mesma máquina carregada, mesmo exe, rodadas seguidas)

Janela da gameplay alinhada pelo vsync do guest (7530→7640), média de `guest_vsync_hz` do `PS2X_FPS_LOG`
(`logs/gsgpu_ab_fps_*.csv`):

| Build | cpu | d3d11 |
|---|---|---|
| antes do item 6 | 5,68 Hz (19 s) | 4,19 Hz (26 s) — **mais lento** |
| atual | 5,95 Hz (19 s) | **6,57 Hz** (17 s), ≈ +10% |
| cena de corte (vsync 7300→7530) | 9,0 s | 6,0 s |

Por quadro de gameplay (log `[gs-gpu]`): ~19 mil primitivas na GPU, **0 no fallback**, ~216 lotes (tex 190, cpu 22,
alias/clut/flush poucos), readback ~4,5 ms por present (~3% do tempo). Preparo das primitivas na CPU e tempo de GPU:
**[NÃO MEDIDO]**. Leitura: a GPU deixou de ser o gargalo; o quadro (~150 ms) está na thread do jogo (VU1 + preparo
de primitivas, que hoje roda inline).

Variáveis de depuração: `PS2X_GS_GPU_VERIFY=N` (a cada N primitivas, CPU confere; log `[gs-gpu-verify]`),
`PS2X_GS_GPU_MAXBATCH=N` (fecha o lote a cada N), `PS2X_GS_GPU_MASK=` bits 1 sprite sem textura, 2 sprite com
textura, 4 triângulo sem textura, 8 triângulo com textura (o resto vai para a CPU).

`PS2X_GS_GPU_HAZARD_LOG=N` imprime (amostrando 1 em 997) os fechamentos de lote por textura com layout e retângulos.

## 5. Pendências (por ganho esperado)

1. **Medir** a thread do jogo em `d3d11` com o amostrador (`PS2X_PROFILE_SECONDS`) para separar VU1 × preparo de
   primitivas × páginas; adicionar query de timestamp para o tempo de GPU.
2. **Thread GS própria** para o preparo e a submissão (fila como a do `cpu-thread`), sobrepondo com o VU1. Ganho
   [NÃO MEDIDO]; só compensa se o preparo for uma fração grande do quadro.
3. Composição PMODE/DISPFB na GPU (E8) para tirar o readback do `Present` (~3%).
4. Fechamentos `cpu` (~22/quadro: uploads/CLUT tocando páginas do lote) e linhas/pontos na GPU (0% medido).
5. Arredondamento: imitar exatamente a ordem de operações da CPU nos triângulos com textura em perspectiva (±1–4).
6. Não testado: outras GPUs (AMD/Intel), adaptador sem ROV (o fallback para a etapa 1 existe mas não foi exercitado),
   perda de device, janela D3D11 visível durante horas.
7. Antes de tornar `d3d11` padrão: decisão do usuário sobre o critério (PSNR em vez de MD5) e rodar mais cenários
   (`explore2`).
