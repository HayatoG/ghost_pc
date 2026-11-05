# IOP em thread própria: análise, desenho e recomendação (05/11/2026, ~05:40)

Pergunta: tirar o IOP (`ps2xIOP`) da thread do jogo para usar mais núcleos e subir o FPS, sem perder o determinismo
(`threading-design.md` seção 10). Prazo curto (35 min): análise primeiro, implementação só se coubesse com validação.

**Conclusão curta:** NÃO implementei a thread do IOP. Ela não compensa: o IOP é ~6,3 % da thread do jogo e quase todo
esse custo é *contabilidade de ocioso* (avançar o relógio do IOP a cada checkpoint do EE), não trabalho de IOP. O mesmo
ganho sai, sem thread e sem risco de determinismo, de um **atalho de ocioso** em `IopEmulator::runEeCycles`, que
implementei (ligado por padrão, `PS2X_IOP_IDLE_FASTPATH=0` desliga). Resultado da validação na seção 6.

## 1. Mapa das interações EE <-> IOP no runtime

Tudo roda hoje na `GameThread` (thread do EE). Arquivos relativos a `work/ps2recomp_game/`.

### 1.1 Relógio: quem avança o IOP e quando
| Ponto | Arquivo:função | Comportamento |
|---|---|---|
| Checkpoint do código recompilado | `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp: EeScheduler::checkpointDue -> accountCycles` | a cada checkpoint do guest (fins de bloco/laço): `m_eeCycle += n`, timers do EE e `m_runtime.advanceIopEeCycles(n)` |
| Espera ociosa do EE | `EeScheduler.cpp` (~2190, `waitForEvent`): laço `accountCycles(step)` | pula o tempo ocioso (relógio só do guest) e leva o IOP junto |
| Ponte | `ps2_runtime.cpp: PS2Runtime::advanceIopEeCycles -> IopSubsystem::runEeCycles -> IopEmulator::runEeCycles` | ciclos do IOP = ciclos do EE / 8 (resto em `eeCycleCarry`) |
| Laço do IOP | `ps2xIOP/src/emulator/iop_emulator.cpp: Impl::runCycles` | por iteração: `servicePendingDmaInterrupts`, `servicePendingGuestCallbacks` (CDVD), `IopTimrman::serviceDue`, `IopKernel::beginNextReady`; sem thread pronta calcula o próximo evento (`nextWakeCycle`, DMA, callbacks, `IopTimrman::nextEventCycle`) e pula; com thread roda `runCpu` (interpretador R3000) até `kDefaultSlice` |

Ou seja: o IOP é **sincronizado por ciclos do EE de granularidade fina** (cada checkpoint), sem thread, sem relógio de
parede. É isso que torna o modo `PS2X_GUEST_CLOCK_ONLY` determinístico.

### 1.2 EE -> IOP (síncronos, na thread do EE)
| Interação | Arquivo:função | Natureza |
|---|---|---|
| SIF RPC call/bind | `ps2_iop_transport.h` -> `PS2Runtime::handleIopRpc` / `canBindIopRpc` / `selectIopRpcAbi` -> `IopRpcBridge::handleRpc` (`ps2xIOP/src/emulator/services/iop_rpc.cpp`) | **síncrono e fora do relógio**: copia o envio da RDRAM para o buffer do servidor no IOP, executa a função do servidor **na hora** (`executeGuestFunction`) e copia a resposta de volta para a RDRAM do EE; o EE recebe a conclusão imediatamente |
| Serviços HLE (libsd, mcserv, cdvd) | `ps2xIOP/src/modules/libsd.cpp` (`IopHost::audioCommand`), `mcserv.cpp` (`IopHost::memoryCard`), `iop_cdvd.cpp` (`openHostFile`/`readHostFile`) | dentro do RPC/import: chamam o host (áudio, memcard, arquivos) na mesma pilha |
| SIF DMA EE->IOP | `Kernel/Stubs/SIF.cpp: sceSifSetDma` (~724) -> `PS2Runtime::writeIopMemory`; também ~44 | **aplicado na hora** ("Transfers are applied immediately by sceSifSetDma in this runtime") |
| SIF DMA IOP->EE lido pelo EE | `SIF.cpp` ~429 -> `readIopMemory` | leitura direta da RAM do IOP |
| Notificação de transferência | `ps2_iop_transport.h` -> `notifyIopSifTransfer` -> `IopSubsystem::onSifTransfer` | serviços; a ponte do emulador ignora (comentário em `iop_rpc.cpp`) |
| Carga/parada de módulos | `Kernel/Syscalls/RPC.cpp` (`loadIopModule`), `System.cpp` (`loadIopModuleBuffer`), `stopIopModule` | executa o `_start` do IRX no IOP, síncrono |
| Memória do IOP | `allocateIopMemory`/`freeIopMemory`/`read/write/zeroIopMemory` | acesso direto às estruturas do IOP |

### 1.3 IOP -> EE (de dentro do `runCycles` ou do RPC, na thread do EE)
| Interação | Arquivo:função | Natureza |
|---|---|---|
| `sceSifSendCmd`/`isceSifSendCmd` no IOP | `iop_rpc.cpp` (import 12/13) -> `IopHost::writeGuest` (dados extras direto na RDRAM) + `PS2IopHostAdapter::sendSifCommand` -> `ps2_stubs::dispatchSifCommand` (`SIF.cpp:229`) | **escreve RDRAM do EE e despacha o handler SIFCMD do EE no meio do laço do IOP** |
| Escritas na RDRAM | `PS2IopHostAdapter::writeGuest/zeroGuest` (`ps2_iop_host.cpp`) | resposta de RPC, extras de SIFCMD |
| Chamada de função do EE | `PS2IopHostAdapter::invokeGuestFunction` | stub (retorna false); não usado |

### 1.4 Eventos internos do IOP (ficam no IOP)
| Item | Onde |
|---|---|
| Interrupções de DMA do IOP (SIF0/1, SPU) | `Impl::schedulePendingDma` (`pendingDmaInterrupts[irq] = totalCycles + delay`) -> `IopIntrman::dispatchInterrupt` |
| Callback de conclusão do CDVD | `pendingGuestCallbacks` com `kCdvdCompletionCycles` |
| Timers (timrman), vblank do IOP | `IopTimrman` (ciclos do IOP), `IopVblank` (import, por `totalCycles`) |
| Semáforos/eventos/threads | `IopKernel` (`dispatchThread/Semaphore/EventImport`) |
| INTC/exceções do R3000 | `Impl::checkInterrupt` (dentro do `runCpu`) |

Não há interrupção de hardware IOP->EE modelada (INTC do EE/DMAC SIF0): a "conclusão" chega ao EE pelo RPC síncrono e
pelo SIFCMD despachado direto.

## 2. Dois desenhos de IOP em thread

### A. Pipeline de fatias fixas (latência determinística de 1 fatia)
O IOP executa a fatia N (ex.: 4 096 ciclos de IOP) na thread IOP enquanto o EE executa a N+1; mensagens só nas
fronteiras: o EE enfileira RPC/SIF DMA/escritas de memória do IOP; o IOP enfileira SIFCMD/escritas na RDRAM; cada lado
aplica a fila do outro no início da sua próxima fatia.

Problemas concretos neste runtime:
- **O RPC é síncrono**: o código do jogo chama `sceSifCallRpc` e espera o resultado na RDRAM no retorno (modo bloqueante)
  ou checa `sceSifCheckStatRpc` (nowait). Para pipeline, o RPC bloqueante teria de virar "EE dorme até a fronteira
  N+2". Isso muda o tempo do jogo (referência de hash muda) e mexe em `RPC.cpp`/transporte e no escalonador do EE
  (`EeScheduler.cpp`, editado por outros agentes agora).
- `sceSifSetDma` "aplicado na hora" teria de ser adiado e o IOP veria os dados uma fatia depois; `dispatchSifCommand`
  teria de ser fila aplicada pelo EE (hoje roda handlers do EE dentro do laço do IOP).
- Handshakes CD/áudio/memcard (EZSOUND, mcserv, cdvd) dependem de ordem RPC -> resposta; com latência fixa a ordem é
  preservada e o resultado é determinístico, mas o tempo muda e casos de "polling com timeout" do jogo podem mudar de
  caminho. Risco médio-alto, validação longa (menu, cena 3D, memcard, FMV, áudio).
- Ganho: só a parte do IOP que é **trabalho real** (interpretador R3000 rodando thread do IOP) sai da thread do EE; a
  contabilidade de fronteira continua. Ver seção 3.

### B. Sincronização imediata nos pontos SIF
O IOP corre à frente numa thread até `eeCycle/8` publicado pelo EE; todo acesso ao estado do IOP (RPC, SIF DMA,
`read/writeIopMemory`, carga de módulo) faz o EE esperar o IOP chegar exatamente ao ciclo do EE e parar (barreira), e todo
SIFCMD/escrita na RDRAM vinda do IOP é enfileirado e aplicado pelo EE no próximo checkpoint **em ciclo exato** (o IOP
não pode passar do ciclo do EE em que a mensagem seria entregue).
- Preserva o comportamento atual quase exatamente (mesma ordem e mesmos ciclos), mas a regra "IOP nunca à frente do EE"
  e a frequência de RPC (áudio/stream do EZSOUND chama RPC várias vezes por quadro) significam muitas barreiras por
  quadro: cada barreira custa uma troca de thread (~2-10 us com evento; spin gasta um núcleo).
- O paralelismo real só existe entre barreiras, e o IOP é ocioso na maior parte do tempo (seção 3): sobra pouco a
  sobrepor.

**Qual preserva melhor o jogo:** B (mesmos ciclos; o pipeline A muda o tempo e exige reescrever o RPC síncrono).
**Qual dá ganho:** nenhum dos dois dá ganho relevante; ver a conta abaixo.

## 3. Quanto há para ganhar (Amdahl)

Perfil `gameplay-profile-hw2-2026-11-05.md`: IOP = **6,3 %** da thread do jogo; as funções quentes são
`IopTimrman::nextEventCycle` 1,6 %, `Impl::runCycles` 1,5 %, `IopTimrman::serviceDue` 1,2 %, `beginNextReady` ~0,4 %.
Nenhuma é o interpretador do R3000: é o custo de **entrar em `runCycles` a cada checkpoint do EE** com poucos ciclos
(n/8) e varrer timers/kernel/DMA para descobrir que não há nada a fazer.

- Teto teórico de mover o IOP inteiro para outra thread: 6,3 % do quadro -> 1/(1-0,063) = **+6,7 % FPS** (~20,3 ->
  ~21,7 vsync/s), e só se a sincronização fosse grátis.
- Realista com B: a barreira por RPC e a publicação do ciclo do EE a cada checkpoint (store atômico, ~0,5-1 % sozinho)
  comem boa parte; estimo **+0 a +3 %**, com risco de perder FPS se as barreiras forem frequentes.
- Realista com A: até ~+5 %, mas muda o tempo do jogo, exige mexer no RPC/escalonador do EE e validação longa; não cabe
  e não justifica o risco agora.
- **Alternativa sem thread (implementada): atalho de ocioso.** Se a última iteração do `runCycles` terminou sem thread
  pronta e nada tocou o IOP desde então, nenhum evento existe antes de `idleUntil` (mínimo de: despertar de thread em
  Delay, IRQ de DMA pendente, callback do CDVD, timer). Avançar `totalCycles` até um valor menor que `idleUntil` é
  exatamente o que o `runCycles` faria (ele só descobriria, varrendo, que não há nada). Remove quase todo o custo
  medido (as três funções quentes) sem thread e sem mudar um ciclo: ganho esperado da ordem de **+3 a +5 %**,
  determinístico por construção.

## 4. O atalho de ocioso (implementado)

Arquivo único: `work/ps2recomp_game/ps2xIOP/src/emulator/iop_emulator.cpp` (nenhum header, nenhum arquivo do runner, sem
reconfigure).
- `Impl::idleUntil` (0 = inválido). No ramo ocioso do `runCycles` o próximo evento passa a ser calculado com fallback
  `UINT64_MAX` em vez de `target` (o `min(target, nextWake)` seguinte é o mesmo valor de antes) e guardado em
  `idleUntil`; quando uma thread roda, `idleUntil = 0`; no `catch` também.
- `IopEmulator::runEeCycles`: se `idleUntil != 0` e `totalCycles + iopCycles < idleUntil`, só soma os ciclos e volta.
  Desigualdade estrita: um evento exatamente em `idleUntil` passa pelo laço normal.
- Invalidação: toda entrada pública que pode mudar estado do IOP vinda do EE (`reset`, `loadModule`, `loadModuleBuffer`,
  `stopModule`, `handleRpc`, `onSifTransfer`, `allocateMemory`, `freeMemory`, `writeMemory`, `zeroMemory`) zera
  `idleUntil` na entrada e na saída (RAII `IdleHorizonReset`; na saída porque uma função do guest executada dentro pode
  reentrar no `runCycles`). Estado do IOP só muda por essas entradas ou pelo próprio `runCycles` (o `IopSubsystem` só
  chama a API pública do `IopEmulator`; os serviços HLE falam com o host, não com o kernel do IOP).
- Desligar: `PS2X_IOP_IDLE_FASTPATH=0` (lido uma vez). Serve de A/B no mesmo binário.

## 5. Recomendação

1. Manter o atalho de ocioso (custo zero de risco se o oráculo bater; seção 6).
2. **Não fazer a thread do IOP** por ora: depois do atalho o IOP deve ficar ~1-2 % da thread do jogo; o teto de uma
   thread vira ~+1-2 % com risco real nos handshakes (RPC síncrono, SIFCMD despachado dentro do laço do IOP). Os alvos
   que valem threads são VU1 (23 %), guest com VU0 macro (14 %) e runtime EE/esperas (`gameplay-profile-hw2` seção 6).
3. Se um dia o IOP voltar a pesar (FMV/áudio em streaming com muito R3000 real), o desenho certo é o **B** (barreira nos
   pontos SIF, IOP nunca à frente do EE), atrás de `PS2X_THREADS=iop`, com o oráculo da seção 10 exigindo hash igual
   ao modo serial (B não muda ciclos, então a referência não deve mudar).

## 6. Validação

Oráculo (`threading-design.md` seção 10): `PS2X_GUEST_CLOCK_ONLY=1`, `PS2X_GS_STREAM_HASH`,
`pad_scripts/determinism_scene3d.txt`, mesmo binário (`work/iopidle_exe/`, build_perf de 05:42), atalho desligado
(`PS2X_IOP_IDLE_FASTPATH=0`) e ligado, corridas simultâneas. Script descartável `work/oracle_idle.ps1`; saídas
`logs/gs_hash_iopidle_{off,on}.txt`, `logs/oracle_iopidle_{off,on}.log`.

| Item | desligado | ligado |
|---|---|---|
| Arquivo de hashes por vsync (7 620 linhas) | md5 `35004f17b285988e2e4d62d0995e760c` | **idêntico** (`cmp` sem diferença) |
| Capturas `d07564` / `d07600` | `267f287b…` / `159bd2c4…` | idênticas |

É o mesmo md5 e as mesmas capturas do registro de `threading-design.md` 10.2 e de `iop-scheduler-opt.md`: o atalho não
muda nenhum ciclo do IOP e o jogo chega à cena 3D. Tempo de parede das duas corridas simultâneas (GS `cpu`, que o
`regress.ps1` usa): 175 s desligado, 169 s ligado (-3,4 %).

### Medição (hw, `work/pad_prof_hw.txt`, `tools/perf_run.ps1` 200 s, mesmo exe, A/B pela env, uma corrida cada)
Script descartável `work/measure_iopidle.ps1`; saídas em `logs/iopidle/`.

| | desligado | ligado |
|---|---|---|
| vsyncs em 200 s | 8 346 | 8 715 (**+4,4 %**) |
| vsync/s médio da corrida | 42,14 | 44,01 (+4,4 %) |
| vsync/s na janela 7530-7640 | 15,44 | 25,08 |
| carga da máquina ao iniciar | 14 % | 34 % |

A janela curta é ruidosa (máquina compartilhada; o "desligado" ficou abaixo dos ~20 vsync/s do perfil hw2, faixa de 9
corridas 18,8-21,0), então **não** tomo o +62 % da janela como ganho. O dado confiável é a média da corrida inteira e o
tempo do oráculo: **+3,5 a +4,5 %**, dentro do previsto pelo perfil (IOP 6,3 %, quase todo contabilidade de ocioso) e
obtido sem thread nova. Falta: perfil novo para confirmar que `nextEventCycle`/`serviceDue`/`runCycles` saíram do topo.

Patch: rodar `tools/sync_patches.ps1` para levar a mudança de `iop_emulator.cpp` ao diretório de patches.
