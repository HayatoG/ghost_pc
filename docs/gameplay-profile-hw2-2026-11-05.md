# Perfil da cena 3D com `PS2X_GS=hw`, rodada 2 (05/11/2026, depois do escalonador do IOP e do corte de quebras de lote)

Objetivo: FPS máximo. Saber ONDE o tempo vai agora que (a) o escalonador do IOP foi otimizado (`iop-scheduler-opt.md`, 22 % -> ~5 %) e (b) o backend hw cortou quebras de lote, readbacks e fallbacks (`gs-hw-backend.md` 5.3).
Só medição: nenhum código do runtime foi alterado, sem ninja/cmake, sem lock. Mesmo método de `gameplay-profile-hw-2026-11-05.md` (amostrador do host na thread do jogo, profile do guest, `work/pad_prof_hw.txt` com `quit` no vsync 9000).
Dados brutos em `logs/gph2/` (fora do git). Nada de bytes do jogo. Sem commit.

## 1. Resumo (vsync/s, onde vai o tempo, o que fazer)

| Item | Valor |
|---|---|
| vsync/s na janela 7530-7640 (sem amostrador pesado) | **~20,3** (9 corridas: 18,8 a 21,0). Antes: 11,6 (perfil anterior); 13,8 depois do IOP |
| Thread do jogo | 90-92 % de um núcleo (era 82 %): continua sendo o gargalo serial; ~10 pontos dos 100 são **espera ociosa do escalonador do EE** (seção 3.3) |
| Maior categoria agora | **VU1** 23,0 % do tempo da thread do jogo (11,3 ms por vsync) |
| Thread de ~61-70 % (antes não identificada) | **thread do driver NVIDIA (OpenGL/DXGK present) atendendo o laço do host sem limite de FPS**; some com `PS2X_TARGET_FPS=60` (seção 2.2). Não é trabalho do jogo |
| Para chegar a 50 vsync/s | cortar **59 %** do tempo da thread (fator 2,46x). VU1 + guest + espera ociosa + runtime EE somados = 58,9 % |

Ranking (detalhes e Amdahl na seção 6): 1) VU1 23,0 %; 2) guest recompilado 14,3 % (11 % só em `FUN_001bf940`/`FUN_001bf590`, cheias de VU0 macro); 3) runtime EE 11,5 %; 4) espera ociosa EE 10,1 %; 5) CRT/locks/heap/QPC fora do módulo ~12 %; 6) GSHwBackend (CPU) 7,5 %; 7) IOP 6,3 %; 8) espelho CPU/upload do GS 6,0 %.

## 2. Como foi medido

| Item | Valor |
|---|---|
| Exe | cópia de `work/ps2recomp_game/out/build_perf/ps2xRuntime/ps2EntryRunner.exe` (05/11 02:47, 131 425 792 bytes, md5 `28971248d5d520eaba2b96b34e770175`) em `work/prof2_exe/` com as DLLs e o `.map` do mesmo build (28 MB). Outro agente reconstruiu/rodou o exe de `build_perf` durante a sessão; as corridas usaram só a cópia |
| Ambiente comum | `PS2X_GS=hw PS2X_GS_SCALE=1 PS2X_GS_WINDOW=0 PS2X_VIDEO_HZ=50 PS2X_TARGET_FPS=0 PS2X_PAD_CLOCK=guest PS2X_THREAD_STATS=1` + (conforme a corrida) `PS2X_HEARTBEAT=1`, `PS2X_GS_HW_VERBOSE=1`. `PS2X_GS_HW_CPUFRAME` não ligado |
| Pad script | `work/pad_prof_hw.txt` (igual ao anterior; `quit` no vsync 9000) |
| Harness | `tools/perf_run.ps1` (CPU por thread, GPU, `PS2X_FPS_LOG`), timeout 200 s, scripts em `logs/gph2/run_*.ps1` |
| Calibração do atraso | `hw2_clean.fps.csv`: vsync acumulado cruza **7530 em t=150,0 s** e 7640 em t=156 s (de novo; o início do gameplay é governado pelo vídeo/abertura, em tempo de parede). Janela de gameplay medida: 7530-7640 (6 s de parede, ~110 vsyncs; o CSV tem resolução de 1 s, então cada taxa tem ±~1 vsync/s) |
| Profile do host (corridas `prof`, `prof2`, `prof3`) | `PS2X_PROFILE_DELAY=149 PS2X_PROFILE_SECONDS=17` (`prof`, `prof2`: t=149-166 s = vsync ~7400-7850) e `DELAY=150 SECONDS=9` (`prof3`: vsync ~7530-7700). 23 505 amostras no total. Resolvido com `tools/resolve_profile.py` + o `.map` do exe medido (esta vez o `.map` foi preservado, lista completa de funções em `logs/gph2/host*_resolved_full.txt`) |
| Profile do guest | `PS2X_GUEST_PROFILE=5` (`guest.txt` = corrida `prof2`, `guest3.txt` = `prof3`), janelas com vsync >= 7600 (a janela anterior contém o fim do vídeo MPEG) |
| Amostrador externo (novo) | `logs/gph2/ext_sampler.ps1`: anexa ao processo, escolhe as threads quentes, SuspendThread/GetThreadContext/ResumeThread (1 kHz na thread do jogo, ~16,7 mil amostras por corrida, perturbação desprezível: 20,5-20,8 vsync/s), RIP -> módulo/export mais próximo, nome (`GetThreadDescription`), endereço de início (`NtQueryInformationThread` classe 9), tempo de criação, e "chamador provável" = primeiro endereço do exe numa varredura de 64 KB da pilha (heurística, ver limitações). Com 3 threads a ~20 kHz cada (corrida `ext2`) a perturbação foi grande (12,2 vsync/s): descartada para taxas |
| Resolução de threads | `GetThreadDescription` só dá nome a `GameThread`; as outras foram identificadas por endereço de início + tempo de criação + onde gastam amostras |

### Carga da máquina (regra de coordenação)

Monitor `logs/gph2/syscpu*.txt` (CIM, a cada 5 s, com a lista de outros `ps2EntryRunner/ninja/cl/link` vivos):

| Corrida | CPU do sistema (média / máx.) | Outro agente |
|---|---|---|
| `clean` (02:51) | 23 % / 43 % | outro `ps2EntryRunner` (build_perf) de 02:51:33 a ~02:55, sobreposto à janela |
| `prof` (02:55) | 22 % / 35 % | outro `ps2EntryRunner` rodando durante toda a corrida |
| `ext`/`ext2` (02:59-03:07) | 24-25 % / 41-50 % | outro `ps2EntryRunner` na maior parte |
| `ext3` em diante (03:07 a 03:55), `prof2`, `prof3`, `clean2`, `nohb*`, `fps60`, `gco*` | 12-17 % / 20-39 % (o próprio jogo é ~11 % = 1,8 núcleos de 16) | nenhum outro `ps2EntryRunner`/ninja/cl/link visto nas amostras de 5 s |

Antes de começar (02:47-02:51) esperei o `ps2EntryRunner` do outro agente terminar (não havia `MAQUINA.lock`; não peguei lock). `clean` (18,8) é a corrida mais ruidosa. Uma corrida falha minha (profile com env errado) foi encerrada por filtro de linha de comando em `powershell` (`perf_run|syscpu|run_ext`); não vi nenhum `perf_run` de outro agente, mas o filtro era amplo.

## 3. vsync/s na janela 7530 -> 7640 (interpolado como em `perf_run.ps1`)

| Corrida | Configuração | vsync/s |
|---|---|---|
| (antes) perfil anterior corrida 1 / 2 | profile do host + guest / só contadores | 11,61 / 10,97 |
| (antes) após o escalonador do IOP | sem / com amostrador (`iop-scheduler-opt.md`) | 13,76 / 15,54 |
| `clean` | heartbeat + verbose, outro agente rodando | 18,82 |
| `clean2` | heartbeat + verbose | 21,01 |
| `nohb` | sem heartbeat/verbose | 20,74 |
| `prof` / `prof2` / `prof3` | profile do host (+ guest) | 19,50 / 20,20 / 20,23 |
| `ext3` / `ext4` / `ext6` / `ext7` | amostrador externo 1 kHz na thread do jogo (`ext7` sem heartbeat) | 20,67 / 20,80 / 20,66 / 20,54 |
| `ext1` / `ext2` | amostrador em 3 threads (a 64 Hz / ~20 kHz) | 17,75 (outro agente rodando) / 12,19 (perturbação) |
| `fps60` / `nohb60` | `PS2X_TARGET_FPS=60` | 21,94 / 22,11 |
| `gco` | `PS2X_GUEST_CLOCK_ONLY=1` (sem heartbeat) | 22,55 |
| `gco60` | `GUEST_CLOCK_ONLY=1` + `TARGET_FPS=60` | 22,47 |

Referência desta rodada: **20,3 vsync/s** (média das 9 corridas sem amostragem pesada). Ganho total sobre o perfil anterior: x1,75 (11,6 -> 20,3). Cada vsync custa ~49 ms de parede (era ~86 ms).
O ganho dos experimentos de ambiente (`fps60`, `gco`) é de +6 a +9 %, no limite da resolução do CSV: indicativo, não conclusivo [repetir com CSV de maior resolução].

### 3.1 Threads do processo (CPU em % de UM núcleo, média de t=151-166 s, `*.cpu.csv`)

| Thread | `clean2` | `nohb` | `ext4` | `ext6` | Papel |
|---|---|---|---|---|---|
| `GameThread` (`ucrtbase+0x2cd00`, criada em 0,78 s) | 90,3 % | 89,8 % | 91,6 % | 89,6 % | EE recompilado + IOP + VU1 + VIF/GIF/DMA + preparo/submissão do backend hw. Amostrada pelo profiler |
| **thread do driver** (início em `nvoglv64.dll+0xa3b690`, criada em 0,66 s) | 63,4 % | 69,5 % | 63,2 % | 62,8 % | ver 3.2 (**é a "thread de ~61 %"**) |
| thread principal (início em `mainCRTStartup`, criada em 0,00 s) | 18,3 % | 16,6 % | 17,0 % | 16,0 % | laço do host (~4 500 iterações/s com `TARGET_FPS=0`, ver `fps` do CSV): 75-93 % das amostras dela em `NtWaitForSingleObject`, ~5-7 % em `NtWriteFile` (heartbeat), resto USER32/win32u |
| worker do UMD D3D11 (início em `nvwgf2umx.dll+0xfd2cfc`) | 5,0 % | 3,5 % | 4,2 % | 3,3 % | submissão de comandos do device D3D11 do backend hw |
| demais | < 1 % | | | | |

Núcleos totais do processo na janela: 1,72-1,84. GPU (maior engine): 13,7-19,3 % (30 % com `TARGET_FPS=60`): folgada.

### 3.2 A thread de ~61-70 %: identificada (medido), causa (medido por experimento)

Amostrador externo (`ext1`, `ext2`, ~350 mil amostras em `ext2`): início em `nvoglv64.dll` (ICD OpenGL/Vulkan da NVIDIA, carregado porque a janela do host é um contexto GL); amostras em `win32u!NtDxgkSubmitPresentToHwQueue` (9-16 %), `NtGdiDdDDIWaitForSynchronizationObjectFromCpu` (10-18 %), `NtGdiDdDDISubmitCommandToHwQueue` (6-8 %), `ntdll!ZwQuerySystemInformation` (12-20 %, sondagem), `NtWaitForSingleObject` (8-11 %), `nvoglv64!DrvPresentBuffers+...` (~24 %). Ou seja: é o worker de apresentação/submissão do driver atendendo o `SwapBuffers` do laço do host.
Experimento (`fps60`, `nohb60`, `gco60`): com `PS2X_TARGET_FPS=60` essa thread cai de 63-70 % para **5-6 %**, o total do processo cai de ~1,8 para ~1,1 núcleo, a thread do jogo sobe para 94-99 % (menos disputa) e o vsync/s sobe ~6 %. Conclusão: com `TARGET_FPS=0` o laço do host gira a ~4 500 quadros/s e gasta ~0,6-0,7 núcleo de driver à toa; não é custo do jogo. [Hipótese não testada: a disputa é por memória/cache/lock do driver entre a thread do jogo e essa thread; o custo na thread do jogo (+6 %) não foi decomposto.] Para uso real, limitar o host a 60 Hz deve ser o padrão.

### 3.3 Espera ociosa do escalonador do EE (novo; medido)

Do amostrador externo na thread do jogo (4 corridas de 16,6-16,7 mil amostras: `ext4`, `ext6`, `ext7` sem heartbeat, `ext8` sem heartbeat e com `TARGET_FPS=60`): **10,1 a 11 %** das amostras estão em `ntdll!NtWaitForAlertByThreadId` (espera de condvar/mutex/WaitOnAddress), e em 98-99 % delas o primeiro endereço do exe na pilha é `EeScheduler::publishSnapshot`. Isso não é disputa de `m_snapshotMutex` (continua igual sem heartbeat e com host a 60 Hz). O código mostra a causa provável: em `EeScheduler::run()` (`src/lib/Kernel/EeScheduler.cpp` ~l.186-190), quando nenhuma thread do guest está pronta, chama `publishSnapshot()` e em seguida `waitForEvent()`, que faz `m_eventCv.wait_until(lock, hostDeadline, ...)` (~l.2135): o host **dorme em tempo real até o próximo evento agendado** (vblank, timers do EE), a menos que `PS2X_GUEST_CLOCK_ONLY` esteja ligado ("idle time is skipped at once"). O endereço de retorno é de `run()` (que inlina `waitForEvent`), por isso a heurística aponta para `publishSnapshot` (chamado imediatamente antes). Confirmação indireta: com `PS2X_GUEST_CLOCK_ONLY=1` a thread do jogo vai a 97-99 % de CPU e o vsync/s sobe +8 %; no mesmo modo a abertura/intro (vsync 7530) chega em 33 s em vez de 150 s.
[NAO MEDIDO] diretamente o tempo dormindo dentro de `wait_until` (só o proxy de amostras); [NAO MEDIDO] o efeito de `GUEST_CLOCK_ONLY` em áudio/sincronização (é o modo do oráculo de determinismo; não foi avaliado como modo de jogo).

## 4. Perfil do host (corridas `prof`, `prof2`, `prof3`, 23 505 amostras na thread do jogo)

### 4.1 Por categoria (% de TODAS as amostras da thread do jogo; as três corridas são consistentes entre si: variação < 1 ponto por categoria)

| Categoria | `prof` (8 917) | `prof2` (9 804) | `prof3` (4 784) | **Total (23 505)** | ms/vsync (x49,3) |
|---|---|---|---|---|---|
| **VU1** (`Native::run` 7,2 %, `finishFmac` 3,1, `execLower` 2,7, `upperFmac` 2,3, `syncSlow` 1,8, `markWrites` 1,7, `execUpper` 1,6, `calculatePairReadyCycle` 0,3) | 23,0 | 23,0 | 22,9 | **23,0** | 11,3 |
| **Guest recompilado** (`FUN_001bf940` 5,6 %, `FUN_001bf590` 5,4 %, resto 3,3) | 14,6 | 14,2 | 14,1 | **14,3** | 7,0 |
| **Runtime EE** (`advanceEeTimers` 3,5 %, `isSpecialAddress` 0,9, `consumeCompletedDmacCauses`, `EeScheduler::checkpointDue/processPendingEvents`, `Ps2*`, MPEG stubs) | 11,7 | 11,6 | 10,9 | **11,5** | 5,7 |
| **GSHwBackend + D3D11GfxDevice** (lado CPU: `SubmitGpuLocked` 2,8 %, `AddPages`, `GetSurfaceLocked`, `UploadPagesLocked`, `MarkWrittenLocked`, `LoadClut`) | 7,3 | 7,7 | 7,4 | **7,5** | 3,7 |
| **IOP** (`IopTimrman::nextEventCycle` 1,6 %, `runCycles` 1,5, `serviceDue` 1,2, `beginNextReady` ~0,4) | 6,2 | 6,5 | 5,9 | **6,3** | 3,1 |
| **Espelho CPU do GS / upload** (`FastUploadLocked` 3,3 %, `UploadPixels` 1,5, `LoadClutMirrorLocked`, `GSMem::ReadTexture`, `GSCpuBackend::LoadClutUnlocked`) | 6,1 | 5,9 | 6,0 | **6,0** | 3,0 |
| **VU0 macro** (`ps2_vu0_update_mac`) | 2,6 | 2,9 | 2,8 | **2,8** | 1,4 |
| **VIF** (`writeUnpackedVectors`, `processVIF1Data`) | 2,2 | 2,4 | 2,0 | **2,3** | 1,1 |
| **GS frontend** (`buildDrawBatch`, `writeRegisterUnlocked`, `processGIFPacket`, `vertexKick`, `GifArbiter`) | 2,2 | 1,9 | 2,3 | **2,1** | 1,0 |
| outros dentro do módulo | 0,6 | 0,6 | 0,6 | 0,6 | 0,3 |
| **Fora do módulo** (espera ociosa, locks, heap, QPC, CRT, driver; ver 4.3) | 23,6 | 23,3 | 25,2 | **23,8** | 11,7 |

Não há função de IOP entre as 20 primeiras além de `nextEventCycle`/`runCycles`/`serviceDue`; `beginNextReady`+`nextWakeCycle` caíram de 25,6 % do módulo para 0,4-0,5 % do total.

### 4.2 Top 20 funções do host (tempo próprio, soma das 3 corridas)

| # | Função | % de todas | % do módulo |
|---|---|---|---|
| 1 | `VU1InterpreterAccess::Native::run` | 7,16 | 9,39 |
| 2 | `FUN_001bf940` (guest) | 5,61 | 7,36 |
| 3 | `FUN_001bf590` (guest) | 5,42 | 7,11 |
| 4 | `PS2Memory::advanceEeTimers` | 3,54 | 4,64 |
| 5 | `GSHwBackend::FastUploadLocked` | 3,25 | 4,26 |
| 6 | `Native::finishFmac` (VU1) | 3,11 | 4,08 |
| 7 | `GSHwBackend::SubmitGpuLocked` | 2,80 | 3,68 |
| 8 | `ps2_vu0_update_mac` | 2,75 | 3,61 |
| 9 | `Native::execLower` (VU1) | 2,74 | 3,60 |
| 10 | `Native::upperFmac` (VU1) | 2,33 | 3,06 |
| 11 | `writeUnpackedVectors` (VIF) | 2,07 | 2,71 |
| 12 | `Native::syncSlow` (VU1) | 1,82 | 2,38 |
| 13 | `Native::markWrites` (VU1) | 1,67 | 2,19 |
| 14 | `IopTimrman::nextEventCycle` | 1,61 | 2,11 |
| 15 | `Native::execUpper` (VU1) | 1,57 | 2,06 |
| 16 | `IopEmulator::Impl::runCycles` | 1,51 | 1,98 |
| 17 | `UploadPixels<...>` (swizzle p/ o espelho) | 1,48 | 1,94 |
| 18 | `IopTimrman::serviceDue` | 1,21 | 1,59 |
| 19 | `PS2Runtime::isSpecialAddress` | 0,88 | 1,16 |
| 20 | `GSHwBackend` `AddPages` (PageSet) | 0,81 | 1,07 |

`FUN_001bf940` e `FUN_001bf590` (guest) são cheias de COP2: o fonte gerado tem 104 e ~40 chamadas a `ps2_vu0_update_mac`. Guest + VU0 macro juntos = 17,1 % (8,4 ms por vsync): é um bloco só do ponto de vista de otimização (macro-ops do VU0 inline na recompilação).

### 4.3 Fora do módulo (23,8 % pelo profile; 25,7 % pelo amostrador externo; composição medida em `ext4/6/7/8`)

| Parcela (% do tempo da thread do jogo) | % | Observação |
|---|---|---|
| Espera ociosa do EE (`NtWaitForAlertByThreadId`, seção 3.3) | 10,1 | tempo real dormindo até o próximo evento |
| `ntdll` restante (heap `RtlAllocateHeap`/`ZwFreeVirtualMemory`, outros) | ~4,9 | chamador não identificado |
| Locks SRW (`RtlAcquire/ReleaseSRWLockExclusive`) | ~2,5 | `std::mutex`/`lock_guard` em caminho quente |
| CRT `VCRUNTIME140` (`memcmp` 0,9, `memmove`, `memset`) + `ucrtbase` | 3,3 | cópias de vértices/páginas, comparações |
| `RtlQueryPerformanceCounter` (1,3) + `GetCurrentThreadId` (0,6) | 1,9 | `steady_clock::now()` e `assertExecutor()` em caminho quente (hipótese: 1 chamada por transferência/ciclo) |
| `MSVCP140` (mutex/shared_ptr spin) | 1,4 | |
| `nvwgf2umx` + `d3d11` (driver D3D11 chamado pela thread do jogo) | 1,5 | pequeno: o custo de driver do jogo é baixo, o grande é a thread de apresentação do host (3.2) |

"Chamador" real dos blocos de heap/locks/QPC/memcmp [NAO MEDIDO]: a heurística de pilha só atribuiu 4 % das amostras de forma confiável (`UpdateBuffer`/`Draw` do `D3D11GfxDevice`, `HwVertex` `vector::insert`, `submit` do `GifArbiter`, `LoadClut` aparecem como "primeiro endereço do exe" nas amostras fora do módulo; ver `ext2.6300.caller.txt`), mas é lida como indício, não prova.

## 5. Guest (`PS2X_GUEST_PROFILE`; transferências entre funções, NÃO tempo)

Janelas com vsync >= 7600 (297 vsyncs em `prof2`, 16,5 mil transferências por vsync; o anterior tinha ~21 mil): o ranking é o mesmo do perfil anterior, o trabalho do guest por quadro não mudou.

| # | Endereço / nome | % |
|---|---|---|
| 1 | 0x100508 `FUN_00100508` | 5,3 |
| 2 | 0x2d72c0 `DIntr` | 5,0 |
| 3 | 0x32bce4 `FUN_0032bce4` | 4,7 |
| 4-7 | 0x1891f0 / 0x189238 / 0x189780 / 0x189290 | 4,2 cada |
| 8 | 0x238128 `FUN_00238128` | 3,8 |
| 9-10 | 0x2dc5fc / 0x2dc60c | 3,1 / 2,9 |
| 11 | 0x2a33d0 `sceVu0ApplyMatrix` | 2,8 |
| 12-14 | 0x32bcfc / 0x2390a4 / 0x239100 | 2,2 / 2,2 / 2,1 |
| 15 | 0x1bf590 `FUN_001bf590` | 2,0 |

Contadores `PS2X_THREAD_STATS` (janelas de 100 vsyncs, `hw2_clean.out.log`): os contadores `r1_*`/`r2_*` impressos são **acumulados** (a coluna "/v" divide o acumulado pelo tamanho da janela), então usei os deltas: MSCAL do VU1 **1 043 por vsync** (7500->7600) e **694 por vsync** (7600->7700); unpacks VIF1 ~3 240 e ~2 150 por vsync; MPG ~100 e ~68 por vsync; PATH2 ~147 e ~97 por vsync. (O perfil anterior citava 1 238-1 737 por vsync lendo o acumulado como taxa: esse número estava superestimado.) A partir do vsync ~7700 os contadores de VU1/PATH1/PATH2 param de crescer (cena parada depois do script do pad), só o PATH3 continua. `vu1_mem_read=0` e `local_to_host_calls=0` continuam: sem leituras do EE à memória do VU1 nem readbacks do GS pelo EE.

## 6. Backend hw: contadores por present (`[gs-hw] presents=...`, `hw2_clean2.out.log`; 1 present ~ 1 vsync de gameplay)

| Contador (por present, intervalo 7500->7800 /300; 7800->8100 igual) | antes (`/300`, intervalo 7200->7500 do perfil anterior) | agora |
|---|---|---|
| `gpu_prims` | 8 500 | 19 060 |
| `api_draws` | 496 | 733 |
| `flush_key` (quebras de lote) | 348 | 400 |
| - por `tex` (troca de textura) | 173 | **0** |
| - por `pso` | 134 | 308 |
| - por `target` | 40 | 87 |
| `dispatches` | 83 | 207 |
| `copies` | 7 | 36 |
| `vtx_mb` | 1,6 | 5,2 |
| `gpu_readback_ms` (acumulado, bloqueia a thread do jogo) | 4 (1 193 ms no intervalo) | **0** (readbacks=0) |
| `cpu_prims` (fallback CPU) | 10 | **0** |

Os dois intervalos não são comparáveis um a um (o anterior misturava o início do desenho com o gameplay e a divisão assumia ~145 presents; este corre inteiro em gameplay e a taxa por present é estável entre 7500->7800 e 7800->8100). O que vale: **tex=0, cpu_prims=0, readbacks=0, gpu_readback_ms=0**: as quebras por textura, o fallback CPU e os readbacks sumiram. Em contrapartida o lote é pequeno: ~733 draws, ~400 quebras (308 por troca de pipeline, 87 por alvo) e ~207 dispatches por vsync (~15 mil draws/s e ~4 mil dispatches/s), cada um com `UpdateBuffer`+`Draw` (`D3D11GfxDevice`), ~5,2 MB de vértices por vsync (~105 MB/s).

## 7. Comparativo antes/depois por categoria (fração do tempo da thread do jogo; ms por vsync de parede)

Antes = `gameplay-profile-hw-2026-11-05.md` (11,6 vsync/s = 86 ms por vsync). Depois = esta rodada (20,3 vsync/s = 49,3 ms por vsync). `*` = o perfil anterior listava só o top 40 (ou só as 2 funções guest): é subestimativa; a "cauda" não listada era ~10 % (8,6 ms).

| Categoria | antes % | antes ms | depois % | depois ms | variação |
|---|---|---|---|---|---|
| Escalonador do IOP | 22,0 | 18,9 | **6,3** | 3,1 | **-15,8 ms** (resto: `IopTimrman` + `runCycles`, não mais o mapa de threads) |
| Fora do módulo (espera, locks, driver, heap) | 25,4 | 21,8 | 23,8 | 11,7 | -10,1 ms (a fração fica igual porque o tempo total caiu; absoluto caiu pela metade) |
| Espelho CPU do GS / upload | 14,0* | 12,0 | **6,0** | 3,0 | -9,0 ms (upload rápido no `GSHwBackend`; `WriteP8`/`DrawSpriteTextured` saíram) |
| VU1 | 12,4* | 10,7 | 23,0 | 11,3 | ~igual em ms (a subestimativa anterior impede dizer mais; **maior fatia agora**) |
| Guest recompilado | 6,8* | 5,8 | 14,3 (2 funções: 10,9) | 7,0 (5,4) | 2 funções: 5,8 -> 5,4 ms (igual) |
| Runtime EE | 3,6* | 3,1 | 11,5 | 5,7 | subestimativa anterior (só `advanceEeTimers`/`isSpecialAddress`/alguns): `advanceEeTimers` 2,8 % do módulo -> 4,6 % |
| GSHwBackend (CPU) | 2,6* | 2,2 | 7,5 | 3,7 | inclui `D3D11GfxDevice` e `PageSet`, que antes caíam na cauda |
| VU0 macro + VIF | 2,9* | 2,5 | 5,1 | 2,5 | igual |
| GS frontend + outros | (na cauda) | | 2,7 | 1,3 | |
| Cauda não listada | ~10 | 8,6 | | | |
| **Total** | 100 | 86,0 | 100 | 49,3 | **-36,7 ms por vsync (x1,75)** |

Do ganho de 36,7 ms: o IOP (-15,8) e o espelho/upload (-9,0) sozinhos explicam 24,8 ms; ~10 ms vêm de "fora do módulo" (menos readbacks/esperas de driver e uma thread menos disputando).

## 8. Ranking atualizado e teto de FPS (Amdahl sobre a thread do jogo)

Base: 20,3 vsync/s = 49,3 ms por vsync, tudo serial na thread do jogo (90-92 % de CPU, resto é a espera ociosa). Teto se o item fosse **zerado** = 20,3 / (1 - fração); "metade" = se o item caísse 50 %. 50 vsync/s exige tirar 59 % do tempo.

| # | Item (o que otimizar) | % medido | ms | Teto se zerado (vsync/s) | Teto se -50 % |
|---|---|---|---|---|---|
| 1 | **VU1** (`Native::run`, `finishFmac`, `execLower`, `upperFmac`, `syncSlow`, `markWrites`): modelo de pipeline por ciclo; ~700-1 000 MSCAL por vsync. JIT/trace de pares, ou caminho rápido sem ciclos exatos quando não há hazard | 23,0 | 11,3 | 26,4 | 22,9 |
| 2 | **Guest + VU0 macro** (`FUN_001bf940`/`FUN_001bf590` + `ps2_vu0_update_mac`): COP2 inline com atualização de MAC/flags a cada instrução; recompilar VU0 macro com flags preguiçosas (só materializar MAC/status quando lidos) | 17,1 (guest 14,3 + VU0 2,8; as duas funções 10,9) | 8,4 | 24,5 | 22,2 |
| 3 | **Runtime EE** (`advanceEeTimers` 3,5 %, `isSpecialAddress` 0,9 %, `consumeCompletedDmacCauses`, `checkpointDue`, `processPendingEvents`): chamados por transferência/ciclo; agrupar/avaliar só em limites de deadline | 11,5 | 5,7 | 22,9 | 21,5 |
| 4 | **Espera ociosa do escalonador do EE** (`waitForEvent`/`wait_until` real-time): não dormir quando o jogo está atrasado em relação ao tempo real; avançar o relógio do guest direto como em `GUEST_CLOCK_ONLY` quando a taxa de vsync < alvo. Zero código de emulação novo | 10,1 | 5,0 | 22,6 (medido com `gco`: 22,5) | 21,4 |
| 5 | **CRT/locks/heap/QPC fora do módulo** (heap por quadro/por draw, `steady_clock::now`, `GetCurrentThreadId` de `assertExecutor`, `memcmp`/`memmove` de vértices, SRW/`std::mutex` em caminho quente): remover alocações e `now()` do laço quente; chamadores [NAO MEDIDO] (precisa de pilha completa, p.ex. ETW) | ~12 (fora do módulo menos a espera e o driver) | 6,0 | 23,1 | 21,6 |
| 6 | **GSHwBackend/D3D11 (CPU)**: ~733 draws, 400 quebras (308 por pipeline, 87 por alvo), 207 dispatches e 5,2 MB de vértices por vsync; `UpdateBuffer`+`Draw` dominam. Ordenar por pipeline, anel de vértices maior/persistente, menos `SetConstants`, multi-draw | 7,5 + driver 1,5 = 9,0 | 4,4 | 22,3 | 21,3 |
| 7 | **IOP** (`IopTimrman::nextEventCycle` 1,6 %, `runCycles` 1,5 %, `serviceDue` 1,2 %): cachear o próximo vencimento de timers (como foi feito com `m_minWake`) | 6,3 | 3,1 | 21,7 | 21,0 |
| 8 | **Espelho CPU/upload** (`FastUploadLocked` 3,3 %, `UploadPixels` 1,5 %, `LoadClutMirrorLocked`): swizzle em compute shader ou upload direto, ~1,1 MB/vsync de PATH3 | 6,0 | 3,0 | 21,6 | 20,9 |
| 9 | VU0 macro (isolado) / VIF / GS frontend | 2,8 / 2,3 / 2,1 | 1,4 / 1,1 / 1,0 | 20,9 / 20,8 / 20,7 | ~20,6 |
| - | Thread do driver a 63-70 % | (fora da thread do jogo) | 0 | +6 % medido com `TARGET_FPS=60` | n/a |

Combinações (mesmo cálculo): VU1 + guest/VU0 + espera ociosa = 50,2 % -> teto 40,8; **+ runtime EE = 61,7 % -> 53** (passaria dos 50 Hz, em tese; na prática o limite é 50 vsync/s). Ou seja, não existe item único que dê 50: são necessários pelo menos VU1, guest/VU0 e EE/espera juntos. Os itens 4 (espera ociosa) e a colocação do host a 60 Hz são de custo quase zero e valem ~+10 % e ~+6 %; o item 5 é barato de medir com ETW e provavelmente de corrigir.

## 9. Arquivos (todos em `logs/gph2/`, fora do git)

- `hw2_{clean,clean2,nohb,nohb60,fps60,gco,gco60,prof,prof2,prof3,ext..ext8}.{cpu.csv,fps.csv,summary.json,out.log}`; `perf_run_hw2_*.log`; `syscpu_*.txt` (carga do sistema), `load.txt`.
- Profile do host: `host.txt` (prof), `host2.txt`, `host3.txt` + `host*_resolved_full.txt`; análise: `analyze_host.py`, `merge_host.py`, `guest_win.py`, `thr.py`; guest: `guest.txt` (prof2), `guest3.txt`.
- Amostrador externo: `ext_sampler.ps1`; saídas `ext*.threads.txt` (resumo por thread, módulo e símbolo), `ext*.<tid>.exe.txt` (histograma RVA do exe), `ext*.<tid>.caller.txt`, `ext*.waitcaller.txt`, `ext*.waitmods.txt`.
- Scripts das corridas: `run_clean.ps1`, `run_prof*.ps1`, `run_ext.ps1`, `run_nohb*.ps1`, `run_fps60.ps1`, `run_gco*.ps1`, `chain*.ps1`.
- Exe medido: `work/prof2_exe/` (cópia isolada, com `.map`).

## 10. Limitações

- Janela de gameplay curta (6 s de parede, ~110 vsyncs; o CSV tem resolução de 1 s: cada taxa tem ±~1 vsync/s). O profile do host cobre 9-17 s (vsync 7530 a 7700-7850); depois de ~7700 a cena fica parada e só o PATH3 continua, mas os percentuais das três corridas coincidem.
- Outro agente rodou jogos em paralelo nas corridas `clean`, `prof`, `ext` e `ext2` (seção 2). Não houve `MAQUINA.lock`.
- O "antes" é do perfil anterior (outro binário; só top 40): a comparação por categoria só é exata para IOP e "fora do módulo". Em VU1, guest, EE e GSHw o "antes" é subestimado.
- A heurística de "chamador provável" lê a pilha acima do RSP e pega o primeiro endereço dentro do exe: pode apontar para um valor residual numa área viva do quadro. Por isso a espera ociosa foi atribuída ao código-fonte por leitura do `EeScheduler::run()`, não pela heurística sozinha.
- `TARGET_FPS=60` e `GUEST_CLOCK_ONLY=1` são experimentos de ambiente (uma corrida cada, ganho no limite da resolução); não foram validados quanto a áudio/sincronização.
- Perfil só da thread do jogo; sem ETW/xperf, sem pilha completa, sem contadores de hardware.
- Cálculos de Amdahl assumem que o item sai do caminho crítico sem custo e que a thread do jogo é o único caminho serial (a GPU está a 14-30 %).
