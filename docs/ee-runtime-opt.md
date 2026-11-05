# Corte do overhead do runtime EE (05/11/2026)

## Causa da espera ociosa
`EeScheduler::waitForEvent` (e `processDueDeadlines`) dormiam até o `hostDeadline` do próximo evento escolhido por ciclos. O
vblank tem prazo de host encadeado (atrasado quando o jogo é lento, então não dorme), mas alarmes e o timer do EE usam
`now() + duração`, sempre no futuro: mesmo com o jogo atrás do tempo real, o EE ocioso dormia em tempo real até eles.

## Mudanças (comportamento preservado quando o host é mais rápido que o jogo)
| Arquivo | Mudança |
|---|---|
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` | `waitForEvent` e `processDueDeadlines`: se algum `VBlankStart` pendente já venceu no relógio do host (jogo atrás do ritmo PS2X_VIDEO_HZ), trata prazos de host como alcançados (mesmo caminho do `PS2X_GUEST_CLOCK_ONLY`: pula o ocioso em ciclos). Quando o jogo está adiantado nada muda: continua dormindo e limitado a 50/60 Hz. `steady_clock::now()` só é chamado quando necessário |
| `ps2xRuntime/src/lib/ps2_memory.cpp` | `advanceEeTimers`: o `unordered_map::find` de GIF_STAT por chamada virou slot em cache (nós estáveis; invalidado no `clear()` do init). Semântica idêntica (a limpeza do FQC continua a cada chamada) |

Não feito (sem tempo): lazy dos timers, atomics em locks, QPC/heap por evento (varredura de chamadores não concluída).

## Validação (exe antes = cópia em `work/ee_before/`, depois = `work/ee_after/`)
- Oráculo (`PS2X_GUEST_CLOCK_ONLY=1` + `PS2X_GS_STREAM_HASH`, `determinism_scene3d.txt`): antes e depois **idênticos**, 7620 linhas, md5 `35004f17...` (igual à referência).
- `regress menu`: byte a byte igual (md5 `84891ae3...`).
- vsync/s na janela 7530-7640 (`PS2X_GS=hw`, `TARGET_FPS=0`, 1 corrida cada, com 2 outros `ps2EntryRunner` na máquina nas duas): antes **18,7**, depois **22,3** (+~20 %, ruidoso; precisa repetir com a máquina parada).
- Build: reconstrução total de 1323 passos (headers de outros agentes), 748 s; log `logs/ee_opt_build.log`.

---

# Rodada 2 (05/11/2026 05:30-06:00): timers lazy, DMAC sem mutex, `now()` sob demanda

## Chamadores reais do "fora do módulo": o que foi (e não foi) possível medir
- ETW com pilha (`wpr`) existe em `C:\WINDOWS\system32\wpr.exe`, mas exige sessão elevada e o tempo (prazo 06:25, máquina com 2-6 `ps2EntryRunner` de outros agentes) não permitia coletar e decodificar o `.etl`. **Chamadores NÃO medidos por pilha.** `xperf` não está instalado.
- Atribuição por leitura de código dos caminhos quentes (por checkpoint = por chamada de função do guest, ~16,5 mil por vsync):
  - `EeScheduler::checkpointDue` -> `accountCycles` -> `PS2Memory::advanceEeTimers` (3,5-4 % do tempo da thread, a maior função do runtime EE) e `runEeCycles` do IOP (já otimizado em `iop-scheduler-opt.md`). Não há `steady_clock::now()`, mutex nem heap nesse caminho: o QPC/locks não vêm daí.
  - `EeScheduler::processDueDeadlines` (por checkpoint pendente): chamava `steady_clock::now()` **antes** de saber se havia prazo vencido (hipótese forte para parte do `RtlQueryPerformanceCounter`, 1,3 %).
  - `PS2Memory::consumeCompletedDmacCauses` (0,67 % + `drainCompletedDmacHandlers` 0,56 %): `std::mutex` + construção/retorno de `std::vector` a cada drenagem (SRW lock e heap) mesmo vazio.
  - Fora do EE (não tocado aqui; heap/locks do backend hw): `HwVertex::vector::insert`, `GifArbiter::submit`, `UpdateBuffer`/`Draw` do D3D11 (indícios de `ext2.6300.caller.txt`).

## Mudanças (comportamento preservado; sem header alterado, rebuild de 6 passos, 16 s)
| Arquivo | Mudança |
|---|---|
| `ps2xRuntime/src/lib/ps2_memory.cpp` | **Timers do EE lazy.** `advanceEeTimers` só acumula os ciclos (`s_eeTimerPending`) e aplica de uma vez quando o acumulado alcança `s_eeTimerSafeCycles` = ciclos até algum timer ativo poder atingir compare **ou** overflow (conservador, ignora flags de habilitação). Antes desse ponto nenhum timer cruza um marco, então aplicar a soma é matematicamente igual a aplicar por chamada (o resto fracionário `clockRemainder` é aditivo). O ponto de cruzamento cai na mesma chamada que antes, logo as interrupções saem no mesmo ciclo. Todo acesso a registrador de timer (leitura/escrita) e `cyclesUntilNextEeTimerInterrupt` fazem `flush` antes; escritas invalidam o limiar; `resetEeTimers` zera o acumulado. A limpeza do FQC do GIF_STAT continua a cada chamada. Estado em variáveis `static` do arquivo (uma instância de `PS2Memory`, como já era o cache do GIF_STAT) para não mexer em `ps2_memory.h` (rebuild total de 12,5 min) |
| `ps2xRuntime/src/lib/ps2_memory.cpp` | `consumeCompletedDmacCauses`: contador atômico estático de causas na fila; fila vazia retorna sem mutex e sem alocar. `queueCompletedDmacCause` incrementa, `init`/`consume` zeram. Uma causa enfileirada por outra thread no instante da checagem é vista na chamada seguinte (antes dependia do mesmo acaso do lock) |
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` | `processDueDeadlines`: `steady_clock::now()` só depois de existir prazo vencido em ciclos e só sem `PS2X_GUEST_CLOCK_ONLY` |

## Validação (exe antes = `work/ee2_before/`, depois = `work/ee2_after/`)
- Oráculo (`PS2X_GUEST_CLOCK_ONLY=1` + `PS2X_GS_STREAM_HASH`, `determinism_scene3d.txt`): antes e depois **idênticos**, 7620 linhas, md5 `35004f17b285988e2e4d62d0995e760c` (igual à referência). Arquivos em `logs/ee2_hash_{before,after}.txt`.
- `regress menu`: PNGs byte a byte iguais entre antes e depois (md5 da lista `b5686af8...`).
- vsync/s no gameplay (janela t=150-158 s do `PS2X_FPS_LOG`, `PS2X_GS=hw`, 2 corridas alternadas cada): antes 18,6 / 18,0 (média 18,3); depois 16,5 / 19,7 (média 18,1). **Sem diferença mensurável**: havia 2 a 6 `ps2EntryRunner` de outros agentes (VU1) rodando durante todas as corridas, ruído (+-2 vsync/s) maior que o ganho esperado (~3-4 % da thread = ~0,7 vsync/s). Precisa repetir com a máquina parada.
- Amostrador do runtime (`PS2X_PROFILE_DELAY=150 SECONDS=9`, exe `work/ee2_prof/` = build_perf 05:47, mesmo código + mudanças de outros agentes; `logs/ee2_prof_host.txt`, resolvido com o `.map`): `advanceEeTimers` **4,05 % -> 0,72 %** das amostras do módulo (de 3,54 % para ~0,54 % de todas as amostras da thread); `consumeCompletedDmacCauses` 0,67 % -> 0,38 %; `drainCompletedDmacHandlers` 0,56 % -> 0,55 %. Fora do módulo: 991 de 3899 amostras = **25,4 %** (antes 23,8-25,2 %): inalterado (esperado: nada de heap/locks do backend hw foi tocado; a máquina carregada também infla a espera). Sem corrida "antes" no mesmo build com amostrador (o `.map` do exe anterior não foi guardado), então a comparação usa o perfil do documento `gameplay-profile-hw2-2026-11-05.md`.

## Pendências
- Chamadores reais de heap/locks/QPC/memcmp: coletar ETW com pilha (`wpr -start CPU -filemode`, terminal elevado, `wpa`/`tracerpt`) com a máquina parada; candidatos do backend hw (`HwVertex` `vector::insert`, `GifArbiter::submit`, `UpdateBuffer`).
- `runEeCycles` do IOP (1,5 % do módulo) e `isSpecialAddress` (1,2 %): próximos itens do runtime EE.
- Repetir o vsync/s com a máquina parada (ganho esperado ~3 % da thread = ~0,6 vsync/s).
- Se der para mexer em `ps2_memory.h`, mover o estado lazy (`s_eeTimer*`, `s_completedDmacCount`) para membros.

