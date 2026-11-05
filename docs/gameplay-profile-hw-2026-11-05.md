# Perfil da cena 3D com `PS2X_GS=hw` (05/11/2026)

Objetivo: saber ONDE o tempo vai agora que o GS deixou de ser o gargalo. Só medição: nenhum código do runtime foi alterado, sem ninja/cmake.
Dados brutos em `logs/gph/` (fora do git). Nada de bytes do jogo.

## Como foi medido

| Item | Valor |
|---|---|
| Exe corrida 1 (perfil) | `work/ps2recomp_game/out/build_perf/ps2xRuntime/ps2EntryRunner.exe` de 05/11 01:52:20 |
| Exe corrida 2 (contadores hw) | mesmo caminho, **recompilado por outro agente às 01:59:28** (não é o mesmo binário da corrida 1) |
| Ambiente comum | `PS2X_GS=hw PS2X_GS_SCALE=1 PS2X_GS_WINDOW=0 PS2X_VIDEO_HZ=50 PS2X_TARGET_FPS=0 PS2X_PAD_CLOCK=guest PS2X_HEARTBEAT=1 PS2X_THREAD_STATS=1`, `PS2X_GS_HW_CPUFRAME` NÃO ligado |
| Pad script | cópia de `regress_scene3d.txt` sem `shot` e com `quit` movido para o vsync 9000 (`work/pad_prof_hw.txt`), para o amostrador não ser cortado pelo `quit` do vsync 7720 |
| Corrida 1 (`logs/gph/hw_prof.*`) | + `PS2X_PROFILE_DELAY=149 PS2X_PROFILE_SECONDS=17` (amostrador do host na thread do jogo) + `PS2X_GUEST_PROFILE=5` (`guest.txt`) |
| Corrida 2 (`logs/gph/hw_verbose.*`) | + `PS2X_GS_HW_VERBOSE=1` (linha `[gs-hw] presents=...` a cada 300 presents), sem amostrador |
| Harness | `tools/perf_run.ps1` (CPU por thread, GPU, `PS2X_FPS_LOG`) |
| Calibração do atraso | `logs/hw/hw_x1_final.fps.csv`: vsync acumulado cruza 7530 em t=150,0 s e 7640 em 159-160 s; confirmado na corrida 1 (t=150,04 e 159,04; 7720 em t=167). Amostrador: t=149 a 166 s (17 s, não 40: o script de pad só tem conteúdo até o vsync ~7700 e depois a cena fica parada) |
| Resolução de símbolos | `tools/resolve_profile.py` com o `.map` de 01:52 (o mesmo do binário medido). O `.map` foi sobrescrito pelo outro agente logo depois; por isso só tenho o resumo "top 40" + por objeto (`logs/gph/host_resolved.txt`); a lista completa de funções não pôde ser regerada |

Carga da máquina durante a medição (CIM `Win32_PerfFormattedData_PerfOS_Processor`, a cada 5 s, `logs/gph/syscpu.txt`): no instante da janela (t≈150-167 s)
o sistema ficou em **10-21 % de 16 threads**, dos quais o próprio jogo usa ~11 % (1,87 núcleos). Média da corrida inteira 19 % (máx. 33 % durante a abertura,
quando o outro agente compilava). `loadavg_hint` ao iniciar: 7 %. Ou seja, na janela de gameplay a máquina estava praticamente livre (outro agente <= ~10 %).
(`Get-Counter` falha neste Windows em português; por isso o CIM.)

## 1. vsync/s na janela de gameplay (vsync acumulado 7530 -> 7640, interpolado como em `perf_run.ps1`)

| Corrida | vsync/s |
|---|---|
| 1 (com amostrador + profile do guest) | **11,61** |
| 2 (só contadores, exe novo) | **10,97** |

Como o guest mede tempo por vsync emulado, ~11 vsync/s = ~22 % do alvo de 50 Hz; cada vsync custa ~90 ms de parede.
Janelas de 5 s do guest profile: 14,6 vsync/s (t 150-155, ainda no início da cena) e ~11 vsync/s depois.

## 2. Perfil do host (corrida 1)

### 2.1 Threads do processo (CPU em % de UM núcleo, média de t=151-167 s, `hw_prof.cpu.csv`; repetido na corrida 2)

| Thread (tid) | corrida 1 | corrida 2 | Papel (inferido; o harness não dá nomes de thread) |
|---|---|---|---|
| 20140 / 22088 | **82 %** | 79 % | **thread do jogo** (EE recompilado + IOP + VU1 + VIF/GIF/DMA + preparo/submissão do backend hw, todos na mesma thread). É a única amostrada pelo profiler |
| 20464 / 17152 | **61 %** | 62 % | hipótese: thread principal de janela/laço do host (gira a ~5 100 quadros/s com `PS2X_TARGET_FPS=0`, ver `fps` do CSV) e/ou thread do driver D3D11. [NAO MEDIDO] qual dos dois: não há nome de thread |
| 17440 / 2708 | 18 % / 12-16 % | 16 % | threads auxiliares (driver/D3D, perfil) [NAO MEDIDO] |
| demais | < 2 % cada | | |

Núcleos totais do processo: 1,87 (corrida 1) e 1,72 (corrida 2); GPU (contador do Windows, maior engine) média **~16 %**: a GPU está folgada.
A thread do jogo está a ~80-88 % de um núcleo: **ela é o gargalo serial**; não é saturação de 100 % porque ~25 % das amostras dela caem fora do módulo (esperas, ver 2.3).

### 2.2 Thread do jogo por categoria (7 482 amostras no módulo + 2 543 fora = 10 025; corrida 1)

| Categoria | amostras | % de todas | % do módulo |
|---|---|---|---|
| **IOP (host)**: `IopKernel::beginNextReady` 1 338, `nextWakeCycle` 578, `IopTimrman::serviceDue/nextEventCycle`, `IopEmulator::runCycles` (objeto `ps2_iop` = 2 203) | 2 203 | **22,0 %** | 29,4 % |
| **Fora do módulo** (kernel/ntdll/driver D3D11/nvidia; esperas, locks, sleeps, chamadas ao driver) | 2 543 | **25,4 %** | n/a |
| **Espelho CPU do GS** (`WriteP8` 350, `DrawSpriteTextured` 312, `UploadImage` 284, `ReadTexture` 98, `WriteP4` 97, `ReadCT32` 84, `LoadClutUnlocked` 61, `ReadCT16` 47, `TexturePageCache::Resolve` 41, `WriteCT16` 34, ...) | 1 408 (top-40) | **14,0 %** | 18,8 % |
| **VU1 nativo/interpretador** (`Native::run` 409, `finishFmac` 197, `execLower` 147, `upperFmac` 142, `syncSlow` 109, `markWrites` 103, `execUpper` 77, ...) | 1 247 (top-40) | **12,4 %** | 16,7 % |
| Código guest recompilado (`FUN_001bf940` 382, `FUN_001bf590` 296) | 678 | 6,8 % | 9,1 % |
| Runtime EE (`advanceEeTimers` 212, `isSpecialAddress` 52, `EeScheduler::*`, `dispatchGuestBranch`) | 360 | 3,6 % | 4,8 % |
| `GSHwBackend` (`SubmitGpuLocked` 142, `AddPages` 46, `GetSurfaceLocked` 37, `LoadClutMirrorLocked` 32) | 257 | 2,6 % | 3,4 % |
| VU0 macro (`ps2_vu0_update_mac`) | 165 | 1,6 % | 2,2 % |
| VIF unpack (`writeUnpackedVectors`) | 132 | 1,3 % | 1,8 % |
| Cauda não listada nos 40 primeiros (varios) | ~1 032 | ~10 % | ~14 % |

Notas: o "VU1 só ~6 %" da conversa anterior não bate com o que vejo nesta build (12,4 %; modelo de pipeline com ciclos exatos continua caro: `finishFmac`,
`markWrites`, `syncSlow`). Atenção: o espelho CPU do GS ainda custa 14 % mesmo com `hw`: é o upload de texturas/imagens para a VRAM-espelho
(212 mil transferências host->local, ~1,2 MB/vsync de path3) e o fallback de CPU para sprites texturizados grandes (`DrawSpriteTextured`).

### 2.3 Top 15 funções do host (tempo próprio, % das amostras do módulo; x0,746 para % de todas)

| # | Função | % módulo |
|---|---|---|
| 1 | `IopKernel::beginNextReady` | 17,88 |
| 2 | `IopKernel::nextWakeCycle` | 7,73 |
| 3 | `VU1InterpreterAccess::Native::run` | 5,47 |
| 4 | `FUN_001bf940` (guest) | 5,11 |
| 5 | `GSMem::WriteP8` | 4,68 |
| 6 | `GSCpuBackend::DrawSpriteTextured<0>` | 4,17 |
| 7 | `FUN_001bf590` (guest) | 3,96 |
| 8 | `GSCpuBackend::UploadImage` | 3,80 |
| 9 | `PS2Memory::advanceEeTimers` | 2,83 |
| 10 | `Native::finishFmac` (VU1) | 2,63 |
| 11 | `ps2_vu0_update_mac` | 2,21 |
| 12 | `Native::execLower` (VU1) | 1,96 |
| 13 | `Native::upperFmac` (VU1) | 1,90 |
| 14 | `GSHwBackend::SubmitGpuLocked` | 1,90 |
| 15 | `writeUnpackedVectors` (VIF) | 1,76 |

### 2.4 Leitura do código do IOP (por que `beginNextReady` é o 1º)

`IopEmulator::Impl::runCycles` (`ps2xIOP/src/emulator/iop_emulator.cpp`, ~l.555) faz, a cada fatia: serviços de DMA/callbacks/timers, depois
`kernel.beginNextReady()` (percorre **duas vezes um `std::map<int, IopThread>` inteiro**: acorda threads em Delay e escolhe a de menor prioridade) e,
quando nenhuma thread está pronta, `nextWakeCycle()` (mais uma varredura do mapa) e avança o relógio até o próximo evento. Com muitas threads IOP
dormindo em atrasos curtos o laço dá muitas voltas por quadro, cada uma com varreduras de mapa (acesso a ponteiros). [NAO MEDIDO] o número de voltas
por segundo (não há contador); a conclusão "é o laço do escalonador, não as instruções do IOP" vem do fato de `runCpu` (instruções IOP) nem aparecer no top 40.

### 2.5 Fora do módulo (25,4 % das amostras da thread do jogo)

[NAO MEDIDO] o detalhamento: o amostrador só grava endereços dentro do módulo. Componentes plausíveis (hipótese): chamadas ao driver D3D11
(`Map`/`Flush`/`Draw`, ~1 000 draws e ~730 trocas de lote por quadro, ver seção 4), espera de `m_mutex` do backend hw (recursivo; a thread principal também o
usa em `Present`), `Sleep`/`WaitForSingleObject` do escalonador do EE, readbacks.

## 3. Guest (`PS2X_GUEST_PROFILE`; transferências entre funções, NÃO tempo)

Janelas t=150-170 s da corrida 1 (3,83 M transferências em ~183 vsyncs, ~21 mil/vsync; a abertura a 50 vsync/s tem ~17 mil/vsync: o trabalho por quadro do guest é parecido, só que agora cada quadro leva ~90 ms).
O trecho inclui ~1,7 % de resto do vídeo MPEG (`sceMpegDemuxPss`) no início da primeira janela.

| # | Endereço / nome | transf. | % |
|---|---|---|---|
| 1 | 0x100508 `FUN_00100508` | 217 862 | 5,7 |
| 2 | 0x2d72c0 `DIntr` | 186 004 | 4,9 |
| 3 | 0x32bce4 `FUN_0032bce4` | 180 188 | 4,7 |
| 4-7 | 0x189238 / 0x189290 / 0x189780 / 0x1891f0 (contagens idênticas) | 158 807 cada | 4,1 cada |
| 8 | 0x238128 `FUN_00238128` | 145 841 | 3,8 |
| 9 | 0x2dc5fc `FUN_002dc5fc` | 117 089 | 3,1 |
| 10 | 0x2dc60c `FUN_002dc60c` | 110 036 | 2,9 |
| 11 | 0x2a33d0 `sceVu0ApplyMatrix` | 105 359 | 2,7 |
| 12 | 0x32bcfc `FUN_0032bcfc` | 83 460 | 2,2 |
| 13 | 0x2390a4 `FUN_002390a4` | 83 139 | 2,2 |
| 14 | 0x239100 `FUN_00239100` | 80 892 | 2,1 |
| 15 | 0x1bf590 `FUN_001bf590` | 78 155 | 2,0 |

Funções de espera/sincronização vistas: `WaitSema` 6 464 chamadas em ~15 s (~35 por vsync; contagem de transferências, não do tempo bloqueado), `SignalSema` 119,
`iSignalSema` 237, `sceIpuSync` 649, `sceCdSync` 111, `sceGsSyncPath` 118; `DIntr` (par DI/EI de seção crítica) ~1 000 por vsync: barato por chamada e 4,9 %
das transferências. **Não há busy-wait de vsync dominante no guest**: o top é lógica de jogo (loop de submissão VIF/DMA 0x1891f0..0x189780, matrizes). O guest ocupa ~7 %
do tempo do host. [NAO MEDIDO] tempo bloqueado em `WaitSema` (o profiler do guest conta chamadas); o `[hb-thr]` do heartbeat poderia mostrar quem espera, não foi analisado.

Contadores de `PS2X_THREAD_STATS` (janelas de ~106-132 vsyncs, 7500->7738): VU1 `MSCAL` **1 238-1 737 por vsync**, unpacks VIF1 3 840-5 388/vsync (242 KB/vsync),
`MPG` 122-171/vsync (200 KB/vsync de microcódigo), PATH1 447-630 pacotes/vsync, PATH2 174-244/vsync, PATH3 0,9-1,2 MB/vsync, `spr_vu0_dma` 51-71/vsync.
`vu1_mem_read=0`, `local_to_host_calls=0`: sem leituras do EE à memória do VU1 nem readbacks do GS pelo EE. Ou seja, são ~1 500 execuções de microprograma VU1 por quadro (~60 us cada em média só no VU1 = ~12 % do tempo).

## 4. Espera/bloqueio do produtor no backend hw (Present, flush, readbacks)

`present_sync_ms` **não existe no backend hw** (só no `[gs-gpu]` do D3D11 ROV): [NAO MEDIDO] diretamente; `PresentSurface` não é cronometrado.
Proxy disponível (corrida 2, `[gs-hw] presents=7200 -> 7500`, intervalo = vsync 7375 -> 7675; o desenho só começa perto do vsync 7530: no intervalo anterior,
6900->7200, só 1 500 prims; então a diferença é quase toda gameplay, ~145 presents; os valores por present abaixo assumem 145 [hipótese de divisão], entre parênteses o dividido por 300):

| Contador | delta no intervalo | por present (/145) | (/300) |
|---|---|---|---|
| `gpu_prims` | +2 563 294 | ~17,7 mil | 8,5 mil |
| `api_draws` | +148 667 | ~1 025 | 496 |
| `flush_key` (quebras de lote) | +104 545 | ~721 | 348 |
| - por `tex` (troca de textura) | +51 872 | ~358 | 173 |
| - por `pso` (pipeline) | +40 178 | ~277 | 134 |
| - por `target` | +11 922 | ~82 | 40 |
| `dispatches` (write-back/load) | +25 000 | ~172 | 83 |
| `copies` | +2 176 | 15 | 7 |
| `vtx_mb` (vértices enviados) | +489 MB | ~3,4 MB | 1,6 MB |
| `gpu_readback_ms` (acumulado, bloqueia a thread do jogo) | **+1 193 ms** | **~8,2 ms** (~9 % do quadro de 91 ms) | 4 ms |
| `cpu_prims` (fallback CPU) | +2 895 | 20 | 10 |

Também: `readbacks` (writebacks) mais `upload_pages` etc. estão na linha completa em `logs/gph/hw_verbose.out.log`.
Taxa de api_draws: ~1 000 por quadro a ~11 quadros/s = ~11 mil draws/s, cada um precedido de `flush_key` com ~720 quebras por quadro: custo de driver na thread do jogo.

## 5. Ranking dos gargalos (fração do tempo da thread do jogo, que é o gargalo serial a ~80-88 % de um núcleo)

| # | Gargalo | % | O que otimizar primeiro |
|---|---|---|---|
| 1 | **Escalonador do IOP host** (`beginNextReady` + `nextWakeCycle` + timers) | **22 %** (medido) | Trocar as varreduras de `std::map` por estrutura incremental (fila de despertar ordenada + conjunto de prontas por prioridade), ou rodar o IOP só quando houver evento/IRQ pendente em vez de a cada fatia; contador de voltas de `runCycles`. É o mais barato de corrigir e o maior item medido |
| 2 | **Fora do módulo: esperas/driver/locks** | 25 % (medido; composição [NAO MEDIDO]) | 1o medir (amostrar também endereços fora do módulo, ou ETW/xperf): hipótese: drivers D3D11 (1 000 draws, 720 quebras de lote/quadro) e `m_mutex` do backend; reduzir quebras de lote por textura/pipeline (358 e 277 por quadro), atlas/array de texturas e ordenação de pipelines |
| 3 | **Espelho CPU do GS** (upload de imagens/VRAM + fallback CPU de sprites) | 14 % (medido) | `UploadImage`/`WriteP8`/`WriteP4` (swizzle na CPU de ~1,2 MB/vsync de PATH3): swizzle em compute shader ou upload direto na GPU sem espelhar na CPU; mover `DrawSpriteTextured` (sprites grandes, ~20/quadro em `cpu_prims`) para a GPU |
| 4 | **VU1** | 12 % (medido) | já foi atacado; sobra `finishFmac`/`markWrites`/`syncSlow` e as ~1 500 execuções de microprograma por quadro; JIT/trace de pares em vez de pipeline por ciclo; menos chamadas de `MSCAL` por quadro é impossível (vem do jogo) |
| 5 | readbacks do backend hw (`gpu_readback_ms`) | ~9 % do quadro (medido, sobrepõe com o item 2) | write-backs pela GPU adiados/em lote; mapear com `staging` assíncrono |
| 6 | guest recompilado | 6,8 % | pouco a ganhar (`FUN_001bf940` e `FUN_001bf590` são os únicos visíveis) |
| 7 | runtime EE (`advanceEeTimers`, `isSpecialAddress`) | 3,6 % | `advanceEeTimers` (2,8 % do módulo) chamado com muita frequência: agrupar |
| 8 | `GSHwBackend` (preparo/submissão) | 2,6 % | não é gargalo na própria lógica; o custo está nas quebras de lote (item 2) |
| 9 | VU0, VIF unpack | 2,9 % | irrelevante |

Hipóteses a validar (não medidas): (a) a thread de ~61 % de CPU (principal) gira a ~5 100 quadros/s por `PS2X_TARGET_FPS=0`; limitar para ~60 poderia liberar um núcleo e reduzir
disputa de `m_mutex`; (b) os 25 % fora do módulo são em parte espera em `m_mutex` recursivo contra `Present` na thread principal; (c) o IOP fica muito mais caro no gameplay que no menu
porque há threads IOP em atrasos curtos (som/CD) rodando em polling; (d) só o item 1 sozinho pode render até ~+25-30 % de vsync/s (22 % do tempo da thread do jogo), o item 3 até +15 %.

## Arquivos (todos em `logs/gph/`, fora do git)
`hw_prof.{out.log,cpu.csv,fps.csv,summary.json}`, `host.txt` (histograma bruto), `host_resolved.txt` (top 40 + objetos), `guest.txt`, `guest_profile_diff.log`,
`syscpu.txt`, `hw_verbose.{out.log,cpu.csv,fps.csv,summary.json}`.

## Limitações
- Duas corridas com binários diferentes (o outro agente recompilou entre elas); o perfil por função é só da corrida 1.
- O amostrador cobre 17 s (vsync ~7530 a ~7700), não 40 s; só a thread do jogo.
- Nomes de threads dos outros tids são inferência. Janela oculta (`PS2X_GS_WINDOW=0`) como nas medições anteriores de `gs-hw-backend.md`.
