# Filtro de tela CRT (pos-processamento na apresentacao)

Escopo: so a **exibicao na janela**. Nada de GS/VRAM: `shot` do pad virtual, `saveGuestFrame` e as capturas de regressao
leem o `frameTex` (pixels do GS) e saem **sem filtro**; o filtro so existe no `DrawTexturePro` da janela. Desligado
por padrao (a imagem e identica a de antes: nenhum shader entra no caminho).

## Onde o quadro chega a janela

- Presenter atual (backends `cpu`, `cpu-thread`): `PS2Runtime::run()` em `ps2_runtime.cpp`: `UploadFrame()` copia o
  `PresentationFrame` (latched no vsync) para `frameTex` (raylib, `FB_WIDTH x FB_HEIGHT`), e `DrawTexturePro(frameTex, srcRect, dstRect)`
  desenha o retangulo visivel (`presentWidth x presentHeight`) com letterbox na janela redimensionavel. O overlay de debug
  (ImGui) e desenhado **depois**, sem filtro.
- Presenter D3D11 do backend GPU (`gs_rhi_d3d11.cpp::PresentTexture`): janela propria, blit com viewport/scissor. **Ainda sem filtro**:
  ver "Slot para o presenter D3D11/RHI" abaixo.

## Design

Um unico passe de fragmento (custo de GPU baixo): ~19 amostras de textura (3 do blur horizontal + 16 de bloom/halacao, estas
dois aneis so se bloom/halation > 0) e algumas dezenas de ALU por pixel de saida.

Ordem das operacoes (tudo em luz linear, gama 2.2):
1. **Curvatura do tubo** (barril): `d' = d * (1 + k*0.25*(dy^2, dx^2))`; fora da area, preto com borda suave de 0,4 %.
2. **Scanlines**: a coordenada vertical e quantizada ao centro da **linha de varredura do original** (`guestLines`: 240/288, ou 224/256
   para quadros 448/512, entrelacado tratado como progressivo; divide-se a escala interna `PS2X_GS_SCALE` antes) e o feixe
   tem perfil gaussiano cuja largura cresce com o brilho do pixel (claros: lacunas pequenas; escuros: lacunas fundas).
   O filtro trabalha em **pixels de saida**: `pxPerLine = outH/guestLines`; abaixo de ~1,5-3,5 px por linha a quantizacao e o
   efeito somem gradualmente (evita moire em janela pequena).
3. **Desfoque horizontal** de composite: 3 taps (+-1 pixel da fonte).
4. Brilho/contraste (pivo cinza 0,18 linear).
5. **Bloom** (anel de 8 taps a 1,5 px) + **halacao** (anel de 8 taps a 5 px): so o que passa de um limiar vaza para os lados.
6. **Mascara de fosforo** em pixels de saida: *aperture grille* (RGB a cada 3 px) ou *slot* (grade com deslocamento a cada linha e
   fresta). Some gradualmente quando ha menos de ~1-2,5 px de saida por pixel da fonte.
7. Vinheta, saturacao, volta a gama, dither de 1/255 (usa o tempo; evita bandas).

## Parametros

| Variavel | Valores | Efeito |
|---|---|---|
| `PS2X_POSTFX_CRT` | `0`/`off` (padrao), `1`/`on`/`medio`, `suave`, `forte` (tambem `soft`/`medium`/`strong`) | liga e escolhe o preset |
| `PS2X_POSTFX_CRT_STRENGTH` | `0..1` (fracao, o que o launcher exporta: suave 0,35, medio 0,65, forte 1,0) ou `0-100` (percentual); valor `<= 1` = fracao, `> 1` = percentual (logo `1` = 100 %, nao 1 %); padrao 100 % | escala todos os efeitos do preset (e o desvio de brilho/contraste/saturacao em relacao a 1); 0 = neutro |
| `PS2X_POSTFX_CRT_MASK` | `grille` (padrao), `slot`, `off` | tipo de mascara de fosforo |
| `PS2X_POSTFX_CRT_LINES` | inteiro | sobrescreve o numero de linhas do original (padrao derivado da altura do quadro) |
| `PS2X_GS_SCALE` | >= 1 | so lido para dividir a escala interna ao derivar `guestLines` |

F9 liga/desliga em tempo de execucao (o filtro precisa ter sido ligado por env; liga/desliga tambem a filtragem bilinear do `frameTex`,
que so existe com o CRT; o `LoadImageFromTexture` dos shots nao e afetado pela filtragem). Valores invalidos geram aviso em stderr
(`[postfx] ...`) e sao ignorados.

Presets (valores a strength 100): suave scan .35 mask .20 curva .03 vinheta .20 bloom .15 blur .20 halacao .10;
medio .60/.40/.08/.35/.30/.35/.20 (+3 % contraste, +5 % saturacao); forte .85/.65/.15/.55/.45/.50/.35 (+2 % brilho, +8 % contraste, +12 % saturacao).
Os valores sao ponto de partida visual, **nao calibrados contra um CRT real** (ver pendencias).

## Arquivos (todos novos, sob `work/ps2recomp_game/ps2xRuntime/`; entram em `patches/` via `tools/sync_patches.ps1`)

| Arquivo | Papel |
|---|---|
| `shaders/gs_postfx_crt.hlsl` | HLSL autocontido (fonte da verdade da matematica); `CrtFilter(Texture2D, SamplerState, uv01)` + `CrtPsMain` opcional (`#define CRT_DEFINE_PS_MAIN`) |
| `shaders/gs_postfx_crt.glsl` | porte GLSL 3.30 (raylib/OpenGL); a mesma matematica e os mesmos 5 uniforms |
| `include/ps2_postfx_crt_shaders.h` | **gerado** por `tools/embed_text.py` (raiz) a partir dos dois: `ps2_postfx::kCrtHlsl`, `kCrtGlsl` |
| `include/ps2_postfx_crt.h`, `src/lib/ps2_postfx_crt.cpp` | config por env (`crtConfigFromEnv`), presets, `computeCrtParams` (funcao pura), `CrtPostFx` (raylib) |
| `tests/postfx_crt_test.cpp` | harness isolado (sem jogo): testa o parser/derivacao de linhas e renderiza um padrao sintetico pelo shader real em PNG |

Regenerar o header depois de editar um shader (da raiz do repo):
`python -I tools/embed_text.py ghost_pc/work/ps2recomp_game/ps2xRuntime/include/ps2_postfx_crt_shaders.h --ns ps2_postfx kCrtHlsl=<...>/shaders/gs_postfx_crt.hlsl kCrtGlsl=<...>/shaders/gs_postfx_crt.glsl`.
Manter HLSL e GLSL em sincronia (mesmos numeros magicos).

## Interface do shader (D3D11, D3D12, Vulkan)

Constantes: **5 `float4`**, mesmo layout em toda API (struct `CrtShaderParams` em `ps2_postfx_crt.h`; preencher com `computeCrtParams(...)`):

| Constante | Conteudo |
|---|---|
| `gCrtSizes` | `inSize.xy` (area visivel em **texels** da textura ligada, ex. 640x448 x escala), `texSize.zw` (textura inteira; pode ser maior) |
| `gCrtOut` | `outSize.xy` (pixels de **saida** do retangulo onde o quadro e desenhado, sem as barras do letterbox), `guestLines` (linhas do original), `time` (s) |
| `gCrtA` | scanline, mask, curvature, vignette (0..1, ja multiplicados pela strength) |
| `gCrtB` | bloom, blur, brightness, contrast |
| `gCrtC` | saturation, maskType (0 nenhuma, 1 grille, 2 slot), halation, reservado |

Entrada: textura RGBA do quadro (qualquer filtro; linear recomendado; o shader limita a amostragem a area visivel) e `uv01` = posicao dentro da
area visivel, 0..1, origem no canto superior esquerdo. Para um quad que mapeia a textura inteira: `uv01 = texUV * texSize / inSize`.
Saida: RGB filtrado, alfa 1, no mesmo espaco de cor da entrada.
Para D3D12/Vulkan: o HLSL compila com DXC (`-spirv` para Vulkan) trocando `cbuffer` por um bloco de constantes/push constants;
`CRT_NO_CBUFFER` permite declarar os cinco `float4` dentro do bloco do presenter. `SampleLevel` e usado em todas as leituras
(legal dentro de ramos).

### Slot para o presenter D3D11/RHI (pendente do agente de GS por hardware)

No passe final (`PresentTexture`): (1) concatenar `ps2_postfx::kCrtHlsl` ao pixel shader do blit e chamar
`CrtFilter(tex, samp, uv01)` no lugar de `tex.Sample(...)`; (2) criar um cbuffer de 80 bytes (5 float4) preenchido com
`computeCrtParams(config, srcWidth, srcHeight, tex.desc.width, tex.desc.height, dstW, dstH, tempo)` (`dstW/dstH` = o retangulo
letterbox ja calculado ali) a cada Present; (3) ligar o filtro linear no sampler quando ativo; `crtConfigFromEnv()` fornece a config
(o `.cpp` do modulo inclui raylib, entao para linkar no backend D3D11 sem raylib mova `crtConfigFromEnv/computeCrtParams` para um
`.cpp` sem raylib: so a classe `CrtPostFx` depende dele). Os capturas de regressao do backend GPU leem a VRAM, nao a janela: sem filtro.

## Integracao (aplicada)

- `CMakeLists.txt`: `src/lib/ps2_postfx_crt.cpp` em `ps2_runtime`.
- `ps2_runtime.cpp`: `#include "ps2_postfx_crt.h"`; `CrtPostFx crtFilter; crtFilter.init(&frameTex, crtConfigFromEnv());` depois de criar o `frameTex`;
  `crtFilter.begin(...)`/`end()` em volta do `DrawTexturePro` do quadro (o overlay de debug fica depois). Desligado, `begin/end` nao fazem nada.
- Lembrete: `tools/sync_patches.ps1` e a tabela de `patches/README.md` (arquivos novos + variaveis `PS2X_POSTFX_CRT*`) ficam a cargo do lider.

## Como validar

- Harness (sem jogo, sem build do runtime): `work\postfx_build.bat` (cl direto; linka o `raylib.lib` ja construido) e depois
  `work\postfx_harness\postfx_test.exe logs\postfx`. Verifica parser (strength 0 = neutro, linhas 240/288/448 etc.), que o GLSL compila e
  renderiza o padrao sintetico com cada preset em PNGs (`logs/postfx/*.png`, nao versionados). O HLSL compila: `fxc /T ps_5_0 /E CrtPsMain /D CRT_DEFINE_PS_MAIN shaders\gs_postfx_crt.hlsl`.
- No jogo: `PS2X_POSTFX_CRT=medio` e olhar a janela; comandos `shot` do pad virtual e as regressoes (`tools/regress.ps1`) devem
  sair identicos com e sem o filtro (o filtro nao toca o `frameTex`); sem a variavel, nada muda.

## Validacao feita na integracao (05/11 01:06)

- Build: ninja incorporou as 3 edicoes (a build de outro agente ja as havia compilado); executavel contem `PS2X_POSTFX_CRT`.
- `regress.ps1 -Scenario menu` com o filtro desligado: IGUAL (md5 84891ae3, diff_px=0).
- Mesmo cenario com `PS2X_POSTFX_CRT=forte` (log `[postfx] CRT filter on (preset=strong ...)`): `shot` IGUAL (md5 84891ae3): o filtro nao toca o `frameTex`.
- **Nao verificado**: a aparencia do filtro na janela real do jogo (so no harness com padrao sintetico).

## Pendencias / limites

- Integracao aplicada em 05/11 (ver "Integracao"). Falta o slot do presenter D3D11 do backend GPU.
- Valores dos presets nao calibrados; nao ha teste A/B no jogo (decisao do usuario).
- Entrelacado: quadros 448/512 tratados como progressivos (sem simular campos); `guestLines` pode ser forcado por env.
- O filtro bilinear do `frameTex` so e ligado quando o CRT esta ativo.
