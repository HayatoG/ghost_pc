# Análise: fim de DMA → handler DMAC (por que o jogo para em `WaitSema`)

Análise estática (2026-11-03) do `ps2xRuntime` em `work/ps2recomp_game/ps2xRuntime` e do código gerado do jogo. Nada foi compilado nem executado. Os números de linha são do estado da árvore no momento da leitura. **Atenção:** a árvore mudou durante a análise. `EeScheduler.cpp`, `ee_scheduler.h` e `ps2_irq_stats.h` já trazem uma "trava" de causa DMAC pendente (`pendingMask`/`latched`/`redelivered`, `redeliverPendingDmac`). Confira se o binário em teste foi compilado antes ou depois dessa mudança.

## 1. O protocolo do jogo (fila de pacotes da GPU)

Variáveis relativas a `gp`: `M = gp-0x325C` (máscara de canais em voo), `DONE = gp-0x4304` (fila ociosa), `Q = *(gp-0x4308)` (fila), `SEMA = gp-0x3258`.

| Função | O que faz |
|---|---|
| `FUN_0019c838` (init) | `CreateSema` → `SEMA`; chama `FUN_0019c8ac` |
| `FUN_0019c8ac` | `AddDmacHandler(2, 0x19ca90, -1)` e `AddDmacHandler(1, 0x19ca90, -1)` (HLE `ps2_syscalls::AddDmacHandler`). Não chama `EnableDmac` |
| `FUN_0019c998` (kick, chamado em `0x19c4ec` por `FUN_0019c3a4`) | `M = 0`; se `Q->count != 0`: `FUN_0019c93c` = `EnableDmac(2)`, `EnableDmac(1)` e **só então** `DONE = 0`; senão: `FUN_0019c968` = `DisableDmac(2)`, `DisableDmac(1)`, `DONE = 1` |
| `FUN_0019ca90(cause)` (handler) | se `DONE != 0`: `EI` e retorna (**IRQ ignorada**); senão `M &= ~(1<<cause)`; se `M == 0`: `FUN_0019caec(Q)` e `EI` |
| `FUN_0019caec(Q)` | se `idx == count`: `DONE = 1`, `iSignalSema(SEMA)` (tail-jump `0x2ced90`). Senão: pacote GIF → `FlushCache`, `sceDmaGetChan(2)`, `sceDmaSend`, **depois** `M |= 4`; pacote VIF1 → idem canal 1, **depois** `M |= 2`; `idx++` |
| `FUN_0019c9f4(Q, 1)` (em `0x19c448`, início do quadro) | se `count != 0`: `WaitSema(SEMA)` ← **onde o jogo para** |
| `FUN_001bac5c` (em `0x19c4fc`, logo após o kick) | `FlushCache(0)`, `sceDmaGetChan(1)`, `sceDmaSend(VIF1)` |

Consequências:
- O primeiro pacote da fila **não** sai pelo kick. Ele sai quando chega uma IRQ de fim de DMA do canal 1 ou 2 depois de `DONE = 0`. No hardware, isso é quase sempre o envio VIF1 de `FUN_001bac5c`, que ainda está em voo.
- Cada bit posto em `M` precisa de exatamente **uma** IRQ correspondente. Se um único `sceDmaSend` não gerar a causa, `M` nunca zera, `caec` nunca roda de novo e `WaitSema` espera para sempre.
- Uma IRQ que chega com `DONE == 1` é descartada. É o comportamento pretendido para os DMAs de fora da fila.

## 2. Trace completo no runtime

### 2a. Escrita direta em CHCR (`sw` no MMIO)
1. Código gerado `WRITE32` → `PS2Runtime::Store32` (`ps2_runtime.cpp:2175`) → `m_memory.write32` → `PS2Memory::writeIORegister`.
2. `D_STAT` (`ps2_memory.cpp:1201-1217`): bits 0..9 são W1C e bits 16..25 alternam a máscara ao escrever 1. O bit 31 é recalculado. `D_PCR` e o `INTC` não entram em lugar nenhum da decisão de entrega.
3. CHCR com STR (`ps2_memory.cpp:1293-1569`):
   - Se `D_CTRL.DMAE == 0`, retorna sem fazer nada (`1297-1302`). O STR fica preso e não há causa.
   - SPR (canais 8/9) → `tryProcessScratchpadDma` → `completeDmacChannel` (`1587-1661`).
   - GIF/VIF1/VIF0 só são tratados se `m_gsVRAM` existir (`1314`). Os outros canais (IPU, SIF…) não fazem nada.
   - Modo normal: `enqueueTransfer(madr, qwc)` **só se qwc > 0** (`1336`, `1556`).
   - Modo chain: percorre as tags (`1408-1522`; `refe`/`end`/`ret` sem ASR encerram; `IRQ && TIE` encerra) e acumula `chainBuf`. Só enfileira **se `chainBuf` não estiver vazio** (`1531`). O CHCR é regravado **com STR ainda ligado** (`1529`).
   - `processPendingTransfers()` é chamado automaticamente, exceto no GIF sem `m_gifPacketCallback`/`m_gifArbiter` (`1561-1566`).
4. `processPendingTransfers` (`1663-1906`): envia GIF/VIF0/VIF1. **Somente se a fila do canal não estava vazia** (`hadGif`/`hadVif0`/`hadVif1`), ele liga o bit em `D_STAT`, faz `queueCompletedDmacCause(n)` e limpa STR e QWC (`1885-1905`).
5. `queueCompletedDmacCause` → vetor `m_completedDmacCauses` protegido por mutex (`1908-1920`).
6. De volta a `Store32`: `drainCompletedDmacHandlers` (`ps2_runtime.cpp:1554-1560`, chamado em `2181`) → `ps2_syscalls::dispatchDmacHandlersForCause` (`Interrupt.cpp:68-71`) → `EeScheduler::dispatchIrq(true, cause)`.

### 2b. `sceDmaSend` HLE (o caminho usado pela fila do jogo)
`sceDmaSend_0x2a15a0.cpp` → `ps2_stubs::sceDmaSend` (`Stubs/DMA.cpp:202`) → `submitDmaSend(..., preferNormalCount=false)` (`Stubs/Helpers/Support.h:1338-1438`):
- CHCR = `0x185 | TTE` (chain, TIE, STR); QWC = 0, MADR = 0, TADR = pacote.
- `writeIORegister(CHCR)` (mesmo caminho de 2a.3) → `processPendingTransfers()` → `consumeCompletedDmacCauses()`.
- Se não vier nenhuma causa **e** o STR já estiver desligado, inventa a causa do canal (`1381-1395`). **Se o STR continuar ligado (nada foi enfileirado), não despacha nada.**
- Despacha as causas **sincronamente**, ainda dentro do `sceDmaSend` (`1432-1435`).

### 2c. Outros produtores
- `kickGifDmaChainFromMMIO` (`ps2_runtime.cpp:2217-2249`): os caminhos nativos de GIF (`tryProcessNativeGif*`, `ps2_memory.cpp:2004-2272`) também enfileiram a causa 2 e drenam.
- `sceGsExecLoadImage`/`StoreImage`/`ResetGraph`/`SyncPath` (`Stubs/GS.cpp:664, 745, 842, 1221`) chamam `processPendingTransfers()` **sem drenar**. A causa só é entregue no próximo `Store32` de qualquer código.
- `Store64`/`Store128` não drenam (`ps2_runtime.cpp:2189-2215`).
- O laço do escalonador **nunca** drena (`EeScheduler.cpp:165-342`).

### 2d. Despacho e execução
- `dispatchIrq` (`EeScheduler.cpp:1294-1358`): conta `dispatch`. Se a causa não está em `m_enabledDmacMask` (padrão `0xFFFFFFFF`, `ee_scheduler.h:425`; muda só por `EnableDmac`/`DisableDmac`), conta `masked` e trava em `pendingMask`. Ignora handlers desabilitados ou sem função recompilada (`not_recompiled`). Sem handler, trava a causa. Monta `GuestInvocation{Interrupt}` com `a0 = cause`, `a1 = arg`, `gp` do registro, `sp = 0`, `ra = 0` → `queueInvocation` (`1111-1117`), que põe em `m_pendingInvocations` e liga `m_checkpointPending`.
- Novas entregas da trava: `redeliverPendingDmac` (`1229-1241`) roda em `addIrqHandler` (`1224`), `setIrqHandlerEnabled(true)` (`1265`) e `setIrqCauseEnabled(true)` (`1288`).
- Execução em `EeScheduler::run`:
  - Com uma thread rodando: em cada checkpoint (`dispatchGuestBranch` → `checkpointDue`, `ps2_runtime.cpp:1427`; arestas de volta; `EeScheduler.cpp:300`), a função gerada retorna ao laço. Na próxima volta, `267-277` empilha a invocação **na thread corrente, se ela não tiver outra invocação ativa**. Como `sp == 0`, recebe `invocationStackTop()` (pilha de 16 KB por thread e profundidade).
  - Com todas as threads bloqueadas (ex.: `WaitSema`): `181-206` usa `acquireInvocationThread()`. O handler roda mesmo com a thread principal bloqueada.
  - Fim: `jr ra` com `ra = 0` → `pc == 0` → desempilha a invocação (`241-258`). `m_insideInterrupt` (`307`) torna `iSignalSema` seguro.
- `EeEventType::Dmac` existe mas o `processEvent` não faz nada nele (`1939-1940`). Hoje nenhum fim de DMA tem latência: a IRQ fica pronta no mesmo instante do `sceDmaSend`.

## 3. Itens verificados e descartados (estático)
- **(e) contexto do handler.** `sp = 0` é substituído por `invocationStackTop()` (`EeScheduler.cpp:201-204`, `271-274`); `gp` vem do `AddDmacHandler` (r28 do chamador, o mesmo `gp` do jogo); `ra = 0` encerra a invocação; `a1` (argumento) é lixo, mas o handler não o usa. **Não quebra.**
- **(d) `EI`/`DI`.** `EI` só faz `cop0_status |= 0x10000`, que ninguém lê. O runtime **ignora o estado de interrupção do guest** e entrega em qualquer checkpoint. Isso não impede o handler de rodar, mas muda a **ordem** (ver H2).
- **(c) thread bloqueada.** Coberta pelo caminho ocioso (`181-206`), desde que a causa já tenha virado invocação. Ver H4.
- **`not_recompiled`.** `FUN_0019ca90_0x19ca90.cpp` existe; esperado 0.
- **Corrida em `caec` (`M |= bit` depois do `sceDmaSend`).** Não ocorre: `caec` só roda dentro da invocação do handler, e `267` só entrega a próxima invocação quando a atual termina.

## 4. Causas prováveis (da mais para a menos provável)

**H1. Um `sceDmaSend` que não gera a causa (fim de DMA perdido).** Se o canal é iniciado mas nada é enfileirado, `processPendingTransfers` não marca o canal. O STR fica ligado, `D_STAT` não muda e `submitDmaSend` não inventa a causa porque o STR continua ligado. Isso acontece com QWC=0 em modo normal, cadeia cujas tags não produzem bytes (ex.: `end`/`ret`/`call` com qwc=0 e TTE desligado), `translateAddress` da tag lançando exceção, tag além da RAM, `m_gsVRAM` nulo ou `D_CTRL.DMAE=0`. Na fila do jogo, um único envio assim deixa um bit preso em `M`: o handler roda para o outro canal, mas `M` nunca chega a 0 e nada sinaliza o semáforo. É exatamente o sintoma "fila parada depois de poucos DMAs" (`dma=6`). No hardware, um DMA vazio termina na hora e levanta CIS.

**H2. Ordem e latência: IRQ entregue cedo demais (DMA síncrono e `DI` ignorado).** No hardware, o envio VIF1 de `FUN_001bac5c` ainda está em voo quando o kick já pôs `DONE = 0`. No runtime, o fim é instantâneo e a invocação entra no checkpoint seguinte. Isso funciona na ordem kick → `1bac5c`, mas qualquer IRQ que encontre `DONE == 1` é **descartada e consumida**. Casos concretos:
- `EnableDmac(2)` dentro de `FUN_0019c93c` entrega a causa travada. O `jal EnableDmac(1)` seguinte é um checkpoint, então o handler roda **antes** de `DONE = 0` (`0x19c954`), vê `DONE = 1` e a trava é limpa sem efeito.
- `redeliverPendingDmac` dentro do `AddDmacHandler` (boot) dispara o handler antes de `Q`/`DONE` existirem. Com `DONE` inicial 0, ele chama `caec(Q)` com `Q` ainda indefinido. Se `idx != count` (lixo), envia pacotes inválidos, liga bits em `M` que nunca serão limpos (H1 de novo) ou dá um `iSignalSema` espúrio.
No hardware, a trava (CIS com CIM=0) só dispara quando a máscara passa de 0 para 1 (`EnableDmac`), nunca no `AddDmacHandler`. Além disso, a máscara de boot é 0, enquanto o runtime começa com `0xFFFFFFFF`.

**H3. Causa mascarada ou sem handler perdida.** Só vale para um binário **anterior** à trava. Os DMAs de boot ou os feitos com `DisableDmac(1/2)` (`FUN_0019c968`) eram descartados (`masked++` sem nova entrega). Na árvore atual isso já está coberto.

**H4. Causa presa em `m_completedDmacCauses`.** Os produtores que não drenam (stubs GS, `Store64`/`Store128`) deixam a causa na fila até o próximo `Store32`. Se a thread principal entra em `WaitSema` e não há outra thread rodando, ninguém mais faz `sw`, e o escalonador vai para `waitForEvent()` sem drenar. Probabilidade menor aqui, porque a fila do jogo usa `sceDmaSend` HLE, que drena por conta própria.

**H5. Handler não recompilado ou contexto errado.** Descartada estaticamente (seção 3).

## 5. Como confirmar com `[hb-irq] dmac …` (PS2X_HEARTBEAT)

| Leitura | Significado |
|---|---|
| `registered=0` | `FUN_0019c838`/`8ac` não rodou: o problema é anterior (inicialização) |
| `registered>=2`, `dispatch=0` | Nenhum fim de DMA foi produzido nem drenado → **H1** (ou H4). Comparar com as linhas `[sceDmaSend] ch=… chcr=…` e `[sceDmaSend:head] … tagQwc/id`: um envio sem `dispatch` correspondente é o culpado (tag com qwc=0, id `end`/`ret`, TTE) |
| `dispatch>0`, `queued=0`, `masked>0` (`last_masked_cause=1/2`) | Fim chegou com a causa desabilitada (`DisableDmac` em `FUN_0019c968`) → H3. Com a trava, `latched>0`; ver se `redelivered>0` |
| `dispatch>0`, `queued=0`, `masked=0`, `latched>0` | Fim chegou antes do `AddDmacHandler` → H3 (a trava deve entregar depois: `redelivered>0`) |
| `not_recompiled>0` | `last_unrecompiled` não tem função gerada (inesperado) |
| `queued>0`, `redelivered>0` e ainda parado | O handler rodou, mas cedo demais (`DONE=1` → descartado) ou com `Q` indefinido → **H2**. Confirmar com um log/breakpoint em `0x19ca90` que registre `DONE` (`*(gp-0x4304)`) e `M` (`*(gp-0x325C)`) |
| `queued>0`, `redelivered=0` e ainda parado; `M != 0` na memória | Handler rodou, mas uma das causas do par nunca veio → **H1** (uma das duas `sceDmaSend` de `caec` não completou) |
| `queued>0`, `M == 0`, `DONE == 1` e ainda em `WaitSema` | A fila acabou e `iSignalSema` foi chamado: o problema está no semáforo, não no DMA |
| `pending_mask` com bit 1/2 sobrando | Uma causa travada que nunca foi entregue (nenhum `EnableDmac` posterior) |

Observação: o `enabled_mask` do heartbeat começa em `0` (valor do `Stats`), mas a máscara real do escalonador começa em `0xFFFFFFFF`. O número só é fiel depois do primeiro `EnableDmac`/`DisableDmac`.

Leituras úteis direto da RAM do guest (painel de debug ou dump) quando parado: `M = *(gp-0x325C)`, `DONE = *(gp-0x4304)`, `Q = *(gp-0x4308)`, `Q->count = Q+0x10`, `Q->idx = Q+0x14`, além de `D_STAT` (`0x1000E010`) e do bit 8 dos CHCR `0x10009000`/`0x1000A000`. **STR ligado em CHCR com o jogo parado confirma H1.**

## 6. Patch proposto (H1 + dreno ocioso de H4); NÃO aplicado

Diff relativo a `ps2xRuntime/`. A ideia: todo início de canal GIF/VIF0/VIF1 termina, mesmo sem dados, como no hardware. Além disso, o escalonador drena causas pendentes antes de dormir.

```diff
--- a/src/lib/ps2_memory.cpp
+++ b/src/lib/ps2_memory.cpp
@@ -1314,10 +1314,12 @@
             if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
             {
+                bool enqueuedAny = false;
                 auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                 {
                     if (qwCount == 0)
                         return;
+                    enqueuedAny = true;
                     const bool scratch = isScratchpad(srcAddr);
                     PendingTransfer pt;
                     pt.fromScratchpad = scratch;
@@ -1531,6 +1533,7 @@
                     if (!chainBuf.empty())
                     {
+                        enqueuedAny = true;
                         PendingTransfer pt;
                         pt.fromScratchpad = false;
                         pt.srcAddr = 0;
@@ -1558,6 +1561,17 @@
                     enqueueTransfer(madr, qwc);
                 }
 
+                if (!enqueuedAny)
+                {
+                    // Nothing to move (QWC=0, empty chain, unreadable tag). Real hardware still ends the
+                    // transfer: STR clears and CIS rises. Without this the channel stays busy forever and
+                    // a DMAC handler waiting for this cause (VIF1=1, GIF=2) never runs.
+                    const uint32_t cause = channelBase == 0x1000A000u ? 2u : (channelBase == 0x10009000u ? 1u : 0u);
+                    completeDmacChannel(channelBase, cause);
+                    return true;
+                }
+
                 const bool autoProcessTransfers =
                     (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                 if (autoProcessTransfers)
--- a/src/lib/Kernel/EeScheduler.cpp
+++ b/src/lib/Kernel/EeScheduler.cpp
@@ -178,6 +178,12 @@
         if (m_currentThreadId == 0)
         {
             GuestThread *next = selectReady();
+            if (!next && m_pendingInvocations.empty())
+            {
+                // Completions queued by paths that do not drain (GS stubs, 64/128-bit stores) must not be
+                // stranded while every thread is blocked on the semaphore their handler would signal.
+                m_runtime.drainCompletedDmacHandlers(m_rdram);
+            }
             if (!next && m_pendingInvocations.empty())
             {
                 copyMainContextToRuntime();
```

**Justificativa.**
- Fecha o único caminho em que um canal 1/2 iniciado nunca gera causa. Com isso, `submitDmaSend` passa a ver STR desligado e a causa na fila, e despacha. O `completeDmacChannel` já existe (`ps2_memory.cpp:1646`) e faz exatamente STR=0, `D_STAT |= 1<<cause` e `queueCompletedDmacCause`. É o mesmo padrão já usado para o SPR.
- O segundo hunk é barato: `drainCompletedDmacHandlers` é público, roda no executor (onde `dispatchIrq` exige estar) e só age quando não há nada pronto.

**Riscos.**
- Jogos que fazem polling em STR esperando que um envio vazio "fique" ocupado. Improvável: no hardware ele termina na hora.
- Fins de DMA de canal 0 (VIF0) passam a gerar causa 0 em casos que antes eram silenciosos. Um handler de causa 0 pode rodar mais vezes, o que é o comportamento correto do hardware.
- O GIF com `autoProcessTransfers == false` não é afetado (há dados enfileirados). Se `m_gsVRAM` for nulo, nada muda: o canal continua sem tratamento.
- O dreno ocioso pode antecipar um handler que antes só rodaria no próximo `Store32`. Isso é desejável, mas muda a ordem relativa a outros eventos.

## 7. Se os contadores apontarem H2 (alternativas, não incluídas no patch)
1. **Entregar a trava só na transição 0→1 da máscara**, como o CIM do hardware. Remover `redeliverPendingDmac` de `addIrqHandler` e de `setIrqHandlerEnabled`, e em `setIrqCauseEnabled` só entregar se o bit estava desligado. Opcionalmente, começar `m_enabledDmacMask` em 0 para os canais 0..9 (no hardware o CIM é 0 no boot). Isso exige que os jogos chamem `EnableDmac`, e todo jogo com handler DMAC chama.
2. **Latência de fim de DMA.** Em `dispatchDmacHandlersForCause`, agendar `scheduleEvent(m_eeCycle + max(kMin, qwc*k), now + eeCyclesToHostDuration(...), EeEvent{EeEventType::Dmac, cause, 0})` e, em `processEvent` (`case EeEventType::Dmac`, hoje vazio), chamar `dispatchIrq(true, event.id)`. A IRQ passa a chegar "depois" do código que o guest executa logo após o envio, como no hardware. É mais invasivo: o CHCR/STR teria de continuar ligado até o evento, senão `sceDmaSync`/`sceGsSyncPath` divergem.
3. **Respeitar o `DI` do guest** (`cop0_status` IE/EIE) antes de entregar invocações `Interrupt` em `run()` (`267`). Fiel ao hardware, mas o código gerado e as HLE não mantêm `Status` de forma consistente, o que pode atrasar IRQs para sempre.
