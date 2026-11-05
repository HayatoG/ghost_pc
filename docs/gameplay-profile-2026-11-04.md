# Perfil da cena 3D de gameplay (04/11/2026)

Objetivo: medir onde o tempo vai na cena 3D de gameplay. Nenhum código-fonte do runtime foi alterado; só medição.
Build usado: `work/ps2recomp_game/out/build_perf/ps2xRuntime/ps2EntryRunner.exe` (não recompilado), `PS2X_GS=cpu`,
pad `pad_scripts/scene3d.txt` (relógio do guest), máquina sem outra carga. Logs e dados brutos em `logs/gp/` (fora do git).

## Corridas feitas

| Corrida | Env além de `PS2X_GS=cpu` | Para quê | Saída |
|---|---|---|---|
| A | `PS2X_GS_FEATURES=1`, `PS2X_GUEST_PROFILE=10` | linha do tempo (t x vsync), contadores GS, perfil do guest | `logs/gp/run_A_features_guest.log`, `guest_A.txt` |
| B | `PS2X_PROFILE_SECONDS=45 PS2X_PROFILE_DELAY=250` | amostrador do host (thread do jogo) | `host_B.txt`, `host_B_resolved.txt`, `host_B_full.txt`, `run_B_host.log` |
| C | `PS2X_GS_DUMP_VSYNC=7570` | dump de 20 000 draws (estatística por quadro) | `gsdump_C.txt`, `run_C_dump.log` |
| D, E | repetição de A (guest) e de B (amostrador) | reprodutibilidade | `guest_D.txt`, `host_E.txt` |
| F | `PS2X_GS=cpu-thread PS2X_GS_DEBUG=1 PS2X_GUEST_PROFILE=10` (exe direto) | rasterizador em thread separada, esperas | `runF_direct.log`, `guest_F.txt` |

Comando base: `..\launcher-rs\target\release\ps2launch.exe run --game . --pad-script pad_scripts\scene3d.txt --timeout 560 --heartbeat`.
Observações de método:
- O `launcher.cfg` tem `renderer = cpu` e **sobrescreve** `PS2X_GS` do ambiente; além disso `--renderer` só aceita `cpu`/`d3d11`.
  Por isso D e E (que eu pretendia em `cpu-thread`) rodaram em `cpu`; viraram repetições. A corrida F foi lançada chamando o
  `ps2EntryRunner.exe` direto com as mesmas variáveis que o launcher usa (`PS2X_PAD_SCRIPT`, `PS2X_HEARTBEAT`, `PS2X_TARGET_FPS=0`, `PS2X_VIDEO_HZ=50`).
- O script `scene3d.txt` termina em `quit` no vsync 7720; a cena de gameplay 3D começa em t≈235 s (vsync ≈7 500) e vai até ≈t 300 s.
- O amostrador do host (`ps2_sampling_profiler`) amostra **só a thread do jogo** (não a thread principal de janela/apresentação, nem a de GS no `cpu-thread`).

## 1. Vsync por segundo na gameplay

Janelas de 10 s do perfil de guest (relógio de parede x contador de vsync do guest):

| Corrida | janela | vsync/s |
|---|---|---|
| A (features+guest) | t=240→300 s, vsync 7562→7718 | **2,6** |
| D (repetição) | t=240→290 s, vsync 7572→7712 | 2,8 |
| F (`cpu-thread`) | t=220→260 s, vsync 7584→7705 | 3,0 |

Medidas médias da corrida inteira (inclui logos, menu, abertura rápida a 50 vsync/s): A 25,7 Hz; B 26,5 Hz; F 29,2 Hz.
**Na gameplay: ~2,6 a 2,8 vsync/s com `cpu` (5 % do alvo de 50 Hz), 3,0 com `cpu-thread`.** Não encontrei 0,2 vsync/s:
o número medido aqui é ~10x maior (pode ser outra cena ou outro pad script). Para chegar a 50 Hz precisa de ~17-19x.

Cada vsync da gameplay corresponde a ~1 quadro apresentado (F: ~3 `Present`/s, ~30 por janela de 10 s) e ~21,7 mil comandos de GS por quadro.
Os vsyncs anteriores (t=100–230 s) rodam a 20-60 vsync/s: o problema é só esta cena.

## 2. Perfil do host por função (thread do jogo, corrida B)

20 476 amostras no módulo + 1 232 fora (kernel/ntdll) = 21 708. Repetição (corrida E, 22 144 + 1 281) deu a mesma distribuição
(DrawSprite 26,8 %, DrawTriangle 18,6 %, `commitReadyPipelines` 11,75 % das amostras no módulo).

| Categoria | amostras | % de todas as amostras | % do módulo |
|---|---|---|---|
| `GSCpuBackend::DrawSprite` | 5 545 | 25,5 % | 27,1 % |
| `GSCpuBackend::DrawTriangle` | 3 738 | 17,2 % | 18,3 % |
| Leitura/escrita de VRAM e textura (`GetReadTextureFunc` lambdas, `TexturePageCache::Resolve`, `WriteP8` etc.) | 1 767 | 8,1 % | 8,6 % |
| Outros do GS CPU (`UploadImage`, etc.) | 267 | 1,2 % | 1,3 % |
| **Rasterizador de GS (soma das 4 linhas)** | **11 317** | **52,1 %** | **55,3 %** |
| **Interpretador VU1** (`VU1Interpreter::*`) | 6 891 | **31,7 %** | 33,7 % |
| IOP (`IopKernel::beginNextReady`, `nextWakeCycle`...) | 1 148 | 5,3 % | 5,6 % |
| Código guest recompilado (`FUN_*`) | 537 | 2,5 % | 2,6 % |
| Runtime EE (timers, memória, escalonador, `Ps2*`) | 399 | 1,8 % | 1,9 % |
| VIF unpack / GIF / DMA | 96 | 0,4 % | 0,5 % |
| VU0 macro | 88 | 0,4 % | 0,4 % |
| Fora do módulo (kernel/ntdll; esperas, sleeps) | 1 232 | 5,7 % | n/a |

Detalhe do VU1: `commitReadyPipelines` 11,5 % (do módulo), `run` 4,2 %, `calculatePairReadyCycle` 2,9 %, `normalizeOperand` 2,7 %,
`rebuildDecodedCodeCache` 2,0 % (recodifica o cache de instruções a cada upload de microprograma), `execUpper` 1,9 %, FMAC flags/resultado ~4 %.
Ou seja, o custo é o modelo de pipeline com ciclos exatos, não as instruções em si.
Os dois únicos guests recompilados visíveis: `FUN_001bf940` (1,05 %) e `FUN_001bf590` (0,77 %).

### Rasterizador numa thread separada? Esperas?
- Com `PS2X_GS=cpu` (padrão): **não**. `GSCpuBackend` roda dentro da própria thread do jogo (EE + VU1 + IOP + raster na mesma thread);
  a thread principal só apresenta o quadro. Por isso as porcentagens acima somam 100 % de **um** núcleo: ~52 % raster, ~32 % VU1.
- Existe `PS2X_GS=cpu-thread` (`GSThreadedBackend`: fila de 1 MB, uma thread de GS). Medido na corrida F (gameplay, janelas de 10 s):
  - Produtor (thread do jogo) bloqueado por fila cheia: ~400 mil esperas x 0,011 ms ≈ **4,4 s por 10 s (~44 %)**; a fila fica no teto (`queue_max_cmds` 3 276, 1 MB).
  - Thread de GS ociosa (`gs_idle_ms`): 3,4 a 4,0 s por 10 s (**~35-40 %**).
  - `sync.present`: ~30 por janela, média 14-23 ms, máx ~100 ms (5-6 % do tempo; `Present` espera a fila drenar).
  - Resultado: 3,0 vsync/s contra 2,6-2,8 (+10 a 15 %). Os dois lados esperam um pelo outro: o jogo gera o quadro em rajadas
    (VU1 produz os comandos, depois o GS consome), então o paralelismo só se sobrepõe parcialmente. Teto teórico com sobreposição perfeita
    ≈ 1/0,52 ≈ 1,9x (≈5 vsync/s), ainda longe de 50 Hz.
- [NAO MEDIDO] custo da thread principal (janela/apresentação) e da thread de GS no amostrador: o amostrador só cobre a thread do jogo.
  Pelos `[perf]`, o laço principal gira a ~1 300-1 600 iterações/s (p50 0,2 ms) e não é gargalo na corrida `cpu`.

## 3. Estatísticas de draws por quadro

### 3a. Dump de 20 000 draws a partir do vsync 7570 (corrida C)
Todos os 20 000 draws caem num único valor de vsync (7570), compatível com ~1 quadro de gameplay (F mostra ~21,7 mil comandos por `Present`).
Pixels são **estimados** pela caixa/área dos vértices recortada a 640x512 (sem scissor, z-test, alpha test), então são um teto.

| Item | Valor |
|---|---|
| Draws | 20 000 (10 550 triângulos 52,8 %; 9 450 sprites 47,2 %) |
| Pixels preenchidos (estimado) | ~7,1 M por esse conjunto (356 px/draw); ~5,3 M nos primeiros 10 709 draws (cena em si) |
| Triângulos: área média / mediana / p90 / p99 | 218 / 4,6 / 167 / 5 334 px (poucos triângulos enormes, muitos minúsculos) |
| Sprites: área média / mediana / p90 / p99 | 511 / 48 / 48 / 8 127 px |
| Texturizados | 97,9 % dos draws, 77,2 % dos pixels |
| T8 (CLUT) | 89,6 % dos draws, 33,0 % dos pixels |
| T4 (CLUT) | 5,4 % dos draws, 2,0 % dos pixels |
| CT32 / CT24 / CT16 | 1,5 % / 1,1 % / 0,2 % dos draws; 20,9 % / 19,0 % / 2,3 % dos pixels |
| Alpha blend (ABE) | 35,8 % dos draws, 61,3 % dos pixels |
| Gouraud / fog | 50,7 % / 49,6 % dos draws |
| Alpha test ligado | 45,1 % dos draws, 52,1 % dos pixels |
| Frame CT32 / outros | 99,7 % / 0,3 % dos draws (outros = 11,4 % dos pixels: CT24 e CT16) |

Estrutura do quadro (por alvo): ~10,4 mil triângulos no buffer principal (fbp 0xA0; 8 961 deles com T8, 6 626 com blend; área média 212 px, ~2,2 M px)
+ ~211 sprites grandes no mesmo alvo (2,2 M px) + ~9 000 sprites minúsculos (área média 45 px, T8) num alvo auxiliar (fbp 0x180),
em ciclos repetidos de 192 draws no 0x180 e 3 no 0xA0 (passes pós-processo/cópia). O custo por draw (setup + cache de textura) pesa mais que o por pixel.

### 3b. Contadores `PS2X_GS_FEATURES` (corrida A, janela dos últimos 600 presents, vsync 6963→7670)
**Ressalva:** a janela é fixa em 600 presents (constante do runtime) e inclui ~500 vsyncs de abertura/lead-in antes da gameplay (a gameplay são só os ~150 últimos vsyncs).
Não é gameplay pura; os números abaixo são indicativos. Total: 2,55 M draws, ~1,04 G px (~409 px/draw).

| Item | draws | pixels |
|---|---|---|
| Triângulo / sprite | 28,3 % / 71,7 % | 38,7 % / 61,3 % |
| Textura T8 / T4 / CT32 / CT24 / CT16 | 93,5 / 3,0 / 1,6 / 0,6 / 0,1 % | 26,0 / 2,7 / 26,2 / 8,6 / 1,8 % |
| CLUT ativa (recarregada) | 96,5 % (6,3 %) | 28,6 % (5,0 %) |
| **Render-to-texture** | **71,6 %** | **37,6 %** |
| Blend: off / simples / aditivo / outro | 80,7 / 14,7 / 3,7 / 0,9 % | 40,5 / 34,3 / 11,9 / 13,4 % |
| Filtro linear | 27,8 % | 43,1 % |
| Frame CT32 | 99,7 % | 86,4 % |
| Z16S com escrita | 28,5 % | 64,0 % |
| Transferências host->local | 212 120 (~220 Mpx, 357 MB) | n/a |
| Leituras local->host | 0 | n/a |

(Heurística de render-to-texture a partir do dump: **[NAO MEDIDO]**. O dump não traz o tamanho dos alvos e minha tentativa por sobreposição de páginas
marcava 98 % dos draws, o que não é confiável. Vale o contador do runtime acima, 71,6 % dos draws / 37,6 % dos pixels.)

[NAO MEDIDO] draws e pixels por segundo estritamente dentro da gameplay: o contador do runtime só imprime a cada 600 presents e o dump é limitado a 20 000 draws.
Estimativa a partir do que foi medido: ~21 mil comandos/quadro x 2,6-3,0 quadros/s ≈ 55-65 mil draws/s; ~7 M px/quadro ≈ 18-21 M px/s.

## 4. Top 10 funções do guest

`PS2X_GUEST_PROFILE` conta **transferências entre funções** (chamadas e tail jumps), não tempo. Soma das janelas t≥250 s da corrida A (gameplay), 2,65 M transferências:

| # | Endereço / nome | transferências | % |
|---|---|---|---|
| 1 | 0x2d72c0 `DIntr` (stub de interrupção) | 136 568 | 5,2 |
| 2 | 0x32bce4 `FUN_0032bce4` | 131 352 | 5,0 |
| 3 | 0x189238 `FUN_00189238` | 115 734 | 4,4 |
| 4 | 0x189780 `FUN_00189780` | 115 734 | 4,4 |
| 5 | 0x189290 `FUN_00189290` | 115 734 | 4,4 |
| 6 | 0x1891f0 `FUN_001891f0` | 115 734 | 4,4 |
| 7 | 0x100508 `FUN_00100508` | 112 396 | 4,2 |
| 8 | 0x238128 `FUN_00238128` | 106 314 | 4,0 |
| 9 | 0x2dc5fc `FUN_002dc5fc` | 85 070 | 3,2 |
| 10 | 0x2dc60c `FUN_002dc60c` | 79 967 | 3,0 |

Seguem `sceVu0ApplyMatrix` (0x2a33d0, 2,9 %), `FUN_0032bcfc`, `FUN_002390a4`, `FUN_00239100`, `FUN_001bf590` (2,1 %).
As quatro funções 0x1891f0..0x189780 têm contagem idêntica (mesma cadeia, uma chamada por item: provável loop de submissão VIF/DMA por pacote).
Importante: o guest recompilado ocupa só ~2,5 % do tempo do host; esta lista mostra o que o jogo faz, não onde o tempo gasta. Nomes não foram renomeados (sem evidência).

## 5. Conclusão: ranking dos gargalos (fração do tempo da thread do jogo, cena de gameplay, `cpu`)

Quadro de ~350 ms (2,6-3,0 vsync/s) contra 20 ms no alvo:

| # | Gargalo | % do tempo | ~ms/quadro | O que fazer |
|---|---|---|---|---|
| 1 | Rasterizador GS em software (DrawSprite 25,5 + DrawTriangle 17,2 + VRAM/textura 8,1 + resto 1,2) | **~52 %** | ~180 | Backend por GPU (D3D11, etapa 2 do design). Cobre ~20 mil draws/quadro, 71 % render-to-texture e 97 % com CLUT T8: precisa de CLUT e render-to-texture desde o início. Inclui os ~9 000 sprites minúsculos de 45 px, onde setup domina. |
| 2 | Interpretador VU1 (modelo de pipeline com ciclos) | **~32 %** | ~110 | Depois do GS, é o próximo: mesmo com GS a custo zero o quadro seguiria em ~170 ms (6 vsync/s). Otimizar `commitReadyPipelines`/`calculatePairReadyCycle`, não reconstruir o cache decodificado a cada upload, ou modo "rápido" sem ciclos exatos / JIT de microprogramas. |
| 3 | IOP (`IopKernel::beginNextReady`) | ~5 % | ~19 | Provável varredura/spin do escalonador do IOP na thread do jogo; checar se dá para dormir até o próximo deadline. |
| 4 | Esperas/kernel fora do módulo | ~6 % | ~20 | Não resolvido; investigar com amostragem das outras threads. |
| 5 | Guest recompilado + runtime EE + VIF/GIF/VU0 | ~5 % | ~17 | Sem prioridade. |

Pontos-chave:
- O jogo é limitado pelo GS em software **e** pelo VU1 ao mesmo tempo (~84 % somados). Só GPU não basta: com o GS em custo zero, o teto é ~170 ms/quadro (~6 vsync/s); para 50 Hz o VU1 tem que cair ~6-8x também.
- Rodar o GS em outra thread (`cpu-thread`) já existe e dá só +10-15 %: o produtor passa ~44 % bloqueado em fila cheia e a thread de GS ~35-40 % ociosa (trabalho em rajadas). Não é o caminho para 50 Hz, mas serve de base para o backend GPU (a fila e os pontos de sincronização já estão lá).
- Ordem sugerida: (1) GPU para GS com CLUT T8/T4 e render-to-texture; (2) VU1 sem ciclos exatos/JIT e fim do rebuild de cache por upload; (3) IOP idle; (4) reavaliar após (1) e (2).
- Reprodutibilidade: A/D/E repetidas deram 2,6-2,8 vsync/s e a mesma distribuição de amostras (±1 ponto percentual).

Limitações: amostrador cobre só a thread do jogo; contador de features mistura gameplay com o lead-in; pixels do dump são estimados;
`launcher.cfg`/launcher não permitem `cpu-thread` (corrida F fora do launcher). Dump de 20 000 draws não distingue fronteiras de quadro.
