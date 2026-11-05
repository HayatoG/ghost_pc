# Threads separadas para EE, VU1 e GS: desenho e plano (05/11/2026)

Decisão do usuário: o port **não** roda numa única thread. EE, VU1 e GS vão para threads separadas, no estilo do
MTVU/MTGS do PCSX2, **sem perder corretude**. Este documento traz o desenho e um protótipo isolado. A implementação no
runtime vem **depois** do VU1 nativo (`vu/`) e do GS por hardware (`gs_hw_*`), que outros agentes estão fazendo agora.
Nenhum arquivo de `work/ps2recomp_game` foi alterado nesta tarefa.

Leitura base: `gameplay-profile-2026-11-04.md` (uma thread do host faz EE + VU1 + IOP + rasterizador),
`gs-threading.md` (modo `cpu-thread`, fila de 32 MiB), `vu1-optimization.md` (replay bit a bit), `gs-gpu-stage2.md`,
`dma-irq-analysis.md`. Os números de linha abaixo são da árvore em 05/11 por volta de 01:00. A árvore muda o tempo todo:
procure pelo nome da função se a linha não bater.

## 0. Resumo

- **4 threads**: **EE** (código do jogo, kernel/escalonador, IOP host, DMAC, parser de VIF1 e de GIF PATH2/3),
  **VU1** (dona da VU1: memória de código, memória de dados e interpretador/nativo), **GS** (dono do frontend GS, do
  `GifArbiter` e do backend) e **janela** (raylib/apresentação, como hoje).
- **3 anéis SPSC**: R1 EE→VU (comandos: UNPACK, MPG, MSCAL/MSCNT, ROW/COL, escrita direta), R2 EE→GS (fluxo GIF
  ordenado: pacotes PATH2/PATH3, **tickets** de chamadas da VU1, marcadores de vsync, pedidos de leitura) e R3 VU→GS
  (pacotes PATH1 de cada chamada, fechados por um marcador de fim).
- **Ordem igual à do modo síncrono, por construção**: hoje cada MSCAL roda inteira, com seus XGKICKs, antes do próximo
  VIFcode. No desenho, o EE põe um ticket em R2 no ponto exato em que a MSCAL rodaria. A thread GS, ao chegar ao ticket,
  consome de R3 a saída daquela chamada. O GS vê **a mesma sequência de pacotes e de drains do arbiter** que vê hoje.
- **Regra de sincronização**: o EE só espera quando **lê** estado que outra thread pode mudar (memória/registradores da
  VU1, VIF1 ROW, CSR/SIGLBLID do GS, VRAM via local→host) ou quando precisa de limite de memória. Fora isso, tudo é
  assíncrono. Fins de DMA continuam instantâneos, como hoje.
- **Protótipo** (`work/threading_proto/`, fora do runtime): o replay das gravações reais de gameplay pelos 3 anéis e 3
  threads produz digests **byte a byte iguais** aos do modo síncrono e aos do `vu1_replay` de referência: 6 000 + 9 000
  chamadas reais e 8 000 sintéticas, com o interpretador e com o executor nativo (`PS2X_VU1_NATIVE=1`), com anel de
  16 MiB e de 256 KiB (seção 9).

## 1. Quem roda onde hoje

| Thread | O que faz | Onde |
|---|---|---|
| `GameThread` | `EeScheduler::run` (código recompilado, syscalls, IOP host), DMAC síncrono, `processVIF1Data`, VU1 (`m_vu1.execute` dentro da MSCAL), `GifArbiter::drain`, `GS::processGIFPacket` (frontend) e, com `PS2X_GS=cpu`, o rasterizador | `ps2_runtime.cpp:2520` |
| principal (janela) | `UploadFrame` → `GS::latchHostPresentationFrame` → `backend->Present`, pad virtual (relógio do guest), heartbeat | `ps2_runtime.cpp:2560-2740`, `:430` |
| GS (`cpu-thread`) | só o backend (raster). O frontend continua na `GameThread` | `gs_thread_backend.cpp` |
| perfil/vigias | amostrador, memwatch | — |

Custos (perfil de 04/11, uma thread a ~350 ms/quadro): raster 52 %, VU1 32 % (depois caiu ~3x com
`vu1-optimization.md`), IOP 5 %, esperas 6 %, guest + runtime EE + VIF/GIF ~5 %.

## 2. Mapa dos pontos de interação EE ↔ VU1 ↔ GS

Classificação no desenho novo:
**A** = assíncrono (vira comando num anel, o EE segue);
**O** = assíncrono, mas a **ordem** tem que ser preservada (ticket/marcador);
**WV** = o EE espera a VU1 esvaziar (R1 vazio e VU ociosa);
**WG** = o EE espera a thread GS processar tudo até aqui;
**WA** = WV seguido de WG;
**L** = local à thread EE, sem mudança.

### 2.1 VIF1 e VU1

| # | Ponto | Código (arquivo:linha) | Hoje | Desenho |
|---|---|---|---|---|
| 1 | DMA VIF1 (canal 1): normal, cadeia, origem SPR | `ps2_memory.cpp:1293-1569` (STR em CHCR, montagem da cadeia em `chainBuf`), `processPendingTransfers` `:1698`, laço VIF1 `:1839-1895` | síncrono: `processVIF1Data` roda tudo na hora | **L** para o parse (o EE continua lendo os VIFcodes); o trabalho vira comandos em R1 |
| 2 | Fim de DMA VIF1 → causa DMAC 1 | `processPendingTransfers` `:1934-1940` → `queueCompletedDmacCause(1)` → `drainCompletedDmacHandlers` (`ps2_runtime.cpp:1636`, chamado em `Store32` `:2263`) | instantâneo, depois do VIF inteiro (incluindo a VU1) | **L**: continua instantâneo, logo depois de enfileirar. O EE não observa a diferença sem passar por um ponto WV (ver 2.4). Preserva o protocolo da fila do jogo (`dma-irq-analysis.md`) |
| 3 | VIF1_FIFO (`sq` em 0x10005000) | `ps2_memory.cpp:1080-1085` | `processVIF1Data` de 16 bytes | **L**, igual a 1 |
| 4 | UNPACK (V1..V4, máscara, modo, ROW/COL, dupla bufferização com TOPS, endereço+TOPS com bit 15) | `ps2_vif1_interpreter.cpp:724-829`, `writeUnpackedVectors` `:55` | escreve direto em `m_vu1Data` | **O/A**: o EE resolve o endereço (TOPS, bit 15), CL/WL e o tamanho; copia o payload (+4 bytes de lookahead do V3-32) e a máscara/modo/COL para R1. A VU1 aplica `writeUnpackedVectors` na memória dela. **ROW** muda com MODE=1/2 (`:234-240`): a cópia autoritativa fica na thread VU1 |
| 5 | UNPACK parcial entre transferências | `ps2_vif1_interpreter.cpp:782-802` (`m_vif1UnpackCommand/Payload`) | junta no EE | **L**: o EE junta e só enfileira o UNPACK completo |
| 6 | STCYCL, STMOD, STMASK, OFFSET, BASE, ITOP, MARK | `:548-592`, `:646-655` | registradores em `vif1_regs` | **L**: estado do parser no EE. STMASK/STMOD/CYCLE vão junto de cada UNPACK |
| 7 | STROW / STCOL | `:656-671` | `vif1_regs.row/col` | **A**: comando `WRITE_ROW/COL` em R1 (como `MTVU_VIF_WRITE_ROW/COL`) |
| 8 | MSCAL / MSCALF / MSCNT (TOP/ITOP, troca de DBF, TOPS) | `:598-645`; callbacks em `ps2_runtime.cpp:725-761` | `m_vu1.execute/resume(..., 65536)` síncrono; depois grava D/T em `vu0_vpu_stat` do contexto corrente | **O**: o EE calcula TOP/ITOP/TOPS/DBF (como hoje) e enfileira `CALL{pc, top, itop, DE/TE de vu0_fbrst}` em R1, **e depois** o ticket em R2. Os bits D/T voltam por flag atômica (2.4 #24) |
| 9 | FLUSH / FLUSHE / FLUSHA | `:594-597` | ignorados (a VU1 já terminou) | **L**: continuam sem efeito. A ordem dos anéis já garante que o que vem depois roda depois da chamada. Num modelo com ciclos, FLUSHA esperaria PATH3, mas isso não é modelado hoje |
| 10 | MPG (upload de microcódigo) | `:672-692`, `markVU1CodeModified` | copia em `m_vu1Code` e invalida o cache | **A**: `WRITE_MICRO{addr, bytes}` em R1. A invalidação do cache (geração) acontece na thread VU1, dona do código |
| 11 | DIRECT / DIRECTHL (PATH2) e IMAGE pendente | `:693-723`, `:499-524`, `pendingGifImageQwc` `:264` | `submitGifPacket(Path2, drain=true)` | **O**: item `GIF{Path2, drain}` em R2, na posição do programa (depois dos tickets anteriores) |
| 12 | MSKPATH3 | `:579-587`, `flushMaskedPath3Packets` (`ps2_memory.cpp:1957`) | `m_path3Masked` + fila de PATH3 retidos | **O**: item `MSKPATH3{on/off}` em R2. A máscara e a fila de retidos passam a ser da thread GS (ver #16 e #18) |
| 13 | VIF1_FBRST (RST/STC) | `ps2_memory.cpp:1227-1242` | zera `vif1_regs`, solta PATH3 | **L** para o parser + **A** `VIF_RESET` em R1 (ROW/COL da VU) + item MSKPATH3 off em R2 |
| 14 | Leitura de registradores VIF1 (STAT, ROW, TOP, ITOP, TOPS… 0x10003C00-0x10003D30) | `readIORegister` (`ps2_memory.cpp:2316`, cai no mapa `m_ioRegisters`, `:2424`) | devolve o último valor escrito (o STAT não reflete `vif1_regs.stat`) | **L** para os registradores do parser; **WV** só para ROW (R0-R3) [NAO VERIFICADO se o jogo lê; contar no passo 0] |
| 15 | Leitura/escrita direta da memória da VU1 pelo EE (0x11008000 código, 0x1100C000 dados) | `mapVuMemory` usado em `read8..read128` (`ps2_memory.cpp:687, 723, 774, 817, 855`) e `write8..write128` (`:883, 927, 991, 1046, 1103`) | acesso direto | leitura **WV**; escrita **A** (`WRITE_DATA/MICRO` em R1, como o PCSX2). [NAO MEDIDO] se o jogo faz isso; contar no passo 0 |
| 16 | DMA de/para SPR com MADR em memória de VU | `resolveVuDmaMemory` `ps2_memory.cpp:1588`, `tryProcessScratchpadDma` `:1614` | memcpy direto | **WV** (antes da cópia), e para VU1 de destino, `WRITE_DATA` em R1 |
| 17 | VPU_STAT (CFC2 vi29) lido pelo guest | inline no código gerado (`SET_GPR_U32(ctx, n, ctx->vu0_vpu_stat)`, 27 arquivos em `src/runner`) | VBS1 sempre 0; D/T gravados no fim da MSCAL | **não interceptável** sem regenerar o código. Neste jogo: os laços de espera testam **bit 0 (VBS0, VU0)** (`FUN_001a7be0`, `entry_001a7ed0`…), o bit 8 (VBS1) só aparece em `entry_002a30fc/0x2a3110` (entradas alternativas do `sceGsSyncPath`, que é HLE) e 0x200/0x2 em funções de depuração da libvu (0x29fxxx-0x2a03xx). Manter VBS1 = 0 ("VU1 instantânea") é seguro **para este jogo**; é risco genérico (seção 8) |
| 18 | VU0 macro / micro (VCALLMS, COP2) | `ps2_runtime.cpp:265-330` (VU0) | síncrono na thread EE | **L**: VU0 fica no EE. [NAO VERIFICADO] se o interpretador da VU0 mapeia registradores da VU1 (no hardware, a VU0 vê os da VU1 em 0x4xxx); se mapear, vira WV |
| 19 | `vu0_fbrst` (DE1/TE1 lidos na MSCAL) | `ps2_runtime.cpp:731-735, 749-753`; escrito inline (`caseD_4_0x29fdc4` etc., máscara 0xC0C) | lido na MSCAL | **A**: os bits vão dentro do comando CALL (como `MTVU_VU_EXECUTE` leva FBRST) |
| 20 | Gravador `PS2X_VU1_RECORD` | `ps2_vu1_core.cpp:72-130` | chamado em `execute/resume` | roda na thread VU1, sem mudança (lê `vsyncTick` atômico) |

### 2.2 GIF e GS

| # | Ponto | Código | Hoje | Desenho |
|---|---|---|---|---|
| 21 | XGKICK → PATH1 | `ps2_vu1_core.cpp:800-905` (`progressXgkick`, fim em `:882-889`: `submitGifPacket(Path1)`), nativo `ps2_vu1_native.cpp:294-305, 801` | `submitGifPacket(Path1, drain=true)` → arbiter → `GS::processGIFPacket` na hora | **O**: a thread VU1 escreve o pacote (já copiado de `m_xgkick.packet`) em R3. A thread GS faz `submit(Path1)+drain()` ao consumir, na posição do ticket |
| 22 | GIF DMA PATH3 (normal, cadeia, SPR) e caminhos nativos | `processPendingTransfers` `:1700-1780` (`submitGifPacket(Path3, drain=false)`, `drain()` em `:1898`), `tryProcessNativeGif*` (`:2039`, `:2231`), `kickGifDmaChainFromMMIO` (`ps2_runtime.cpp:2300`) | síncrono | **O**: itens `GIF{Path3, drain=false}` + `DRAIN` em R2, na mesma sequência de hoje. Os caminhos nativos que consultam `m_path3Masked` precisam da máscara "como seria agora": o EE mantém uma **cópia espelho** da máscara, atualizada pelo próprio parser de VIF (#12), que é quem a muda. Fica exata porque só o VIF1 (no EE) muda a máscara |
| 23 | `GifArbiter` (ordenação estável por caminho em cada drain) | `ps2_gif_arbiter.cpp:29-73`; `t_currentPath` é `thread_local` (`:7`) | thread do produtor | vai inteiro para a thread GS. Como a thread GS recebe os mesmos submits e drains na mesma ordem, a ordenação estável dá o mesmo resultado. `thread_local` passa a valer na thread GS, que é quem chama o frontend |
| 24 | SIGNAL / FINISH / LABEL (CSR, SIGLBLID) | `gs_frontend.cpp:1660-1695` (`csr.fetch_or`, `siglblid` não atômico) | gravados pelo frontend na thread do produtor | frontend na thread GS: **CSR e SIGLBLID ficam atrasados**. Leitura de CSR/SIGLBLID pelo EE (`read32/64`, `ps2_memory.cpp:745-755, 793-799`) vira **WA**. Opção 2 (se a contagem mostrar que é quente): pré-varredura de A+D no produtor, como o `Gif_HandlerAD` do PCSX2 (seção 5) |
| 25 | Escrita do EE no CSR (W1C de SIGNAL/FINISH, RESET bit 9) | `writeCsrHalf/Full` (`ps2_memory.cpp:108-141`, `:962, :1021`) | atômico | **WA** antes da escrita (para não apagar um FINISH que ainda vai chegar e o EE esperaria). RESET do GS: item `GS_RESET` em R2 |
| 26 | Registradores privilegiados de exibição (PMODE, DISPFB1/2, DISPLAY1/2, BGCOLOR, SMODE…) | `gsRegPtr` (`ps2_memory.cpp:94`), escritos pelo EE; lidos em `buildPresentationRequestUnlocked` (`gs_frontend.cpp:523`) **pela thread da janela** | corrida benigna hoje | **O**: o EE tira uma cópia deles no VBlankStart e manda num item `VSYNC{tick, priv regs}` em R2. A apresentação usa essa cópia: deixa de depender do instante em que a janela acorda |
| 27 | TRXDIR local→host (`sceGsExecStoreImage`) | `Stubs/GS.cpp:720-750` (`consumeLocalToHostBytes` `:748`), `GS::consumeLocalToHostBytes` (`gs_frontend.cpp:1825`), `GSThreadedBackend::ConsumeLocalToHostBytes` `:254` | síncrono (com `cpu-thread`, `RunOnGsThread`) | **WA** e depois leitura. Medido em 04/11: 0 leituras local→host na gameplay (`gameplay-profile` 3b) |
| 28 | `sceGsSyncPath`, `sceDmaSync`, `sceGsSyncV`, `sceGsExecLoadImage`, `sceGsResetGraph` | `Stubs/GS.cpp:664, 745, 842, 1214, 1281`; `submitDmaSync` (`Helpers/Support.h:1440`) | HLE: `processPendingTransfers` e espera STR=0 | **L**: o DMA continua terminando na hora. `sceGsSyncPath` **não** precisa esperar o GS (no hardware espera os caminhos terminarem, mas o EE não observa a VRAM sem #27). `ResetGraph`: item `GS_RESET` + **WA** |
| 29 | Leitura de CHCR (STR) e D_STAT | `readIORegister` `ps2_memory.cpp:2393-2402` (leitura de CHCR **apaga** STR), D_STAT `:1201-1217` | DMA sempre "pronto" | **L**: sem mudança |
| 30 | GIF_STAT (M3P, FQC) | `ps2_memory.cpp:2377-2391` | M3P = `m_path3Masked` | **L** com a cópia espelho de #22 |
| 31 | `Present` / `latchHostPresentationFrame` | `ps2_runtime.cpp:430-450`, `gs_frontend.cpp:544-593` | thread da janela pega `m_stateMutex` do frontend e chama `backend->Flush/Sync/Present` | a janela **não toca** no frontend: a thread GS, ao processar `VSYNC{n}`, produz o quadro n num buffer triplo; a janela só mostra o último pronto. A captura de tela (pad `shot`) passa a ser do quadro do vsync n exato (determinística) |
| 32 | Painel de depuração / snapshots de VRAM | `gs_frontend.cpp:217-225` (`Sync(DebugReadback)` + `SnapshotVram`), `ps2_debug_panel.cpp` | já é racy hoje | pedido em R2 com resposta (como `RunOnGsThread`); só depuração |

### 2.3 Tempo e relógio do guest

| # | Ponto | Código | Observação |
|---|---|---|---|
| 33 | VBlank | `EeScheduler::processDueDeadlines` (`EeScheduler.cpp:1846-1930`), `processEvent` VBlankStart `:1939-1977` | o evento dispara quando **os dois** valem: `m_eeCycle ≥ deadlineCycle` **e** relógio do host ≥ `hostDeadline`. Quando o jogo está mais lento que o tempo real (hoje, 3 vsync/s), quem manda é o contador de ciclos do EE: o vsync é determinístico em ciclos do guest |
| 34 | Ciclos do EE | `checkpointDue` `:367`, `accountCycles` `:401` (32 ciclos por checkpoint gerado) | a VU1 **não** soma ciclos ao EE hoje (VU1 "instantânea") |
| 35 | Timers do EE (T0-T3) | `advanceEeTimers` (`ps2_memory.cpp:407`, chamado em `EeScheduler.cpp:405`) | avançam com os ciclos do EE: determinísticos, ficam no EE |
| 36 | Pad virtual com relógio do guest | `ps2_runtime.cpp:2720-2740` (`vsyncTick` atômico, lido na thread da janela) | o pad aplica o script pelo vsync do guest, mas **na thread da janela**, a ~1 300 iterações/s. Com o EE mais rápido, pode pular frames do script. Ver seção 3.6 |
| 37 | Callbacks de vsync, alarmes, IRQ INTC 2/3 | `processEvent` `:1957-1997` | todos na thread EE |

### 2.4 Interrupções

| # | Fonte | Hoje | Desenho |
|---|---|---|---|
| 38 | Fim de DMA (DMAC 0/1/2/8/9) | fila `m_completedDmacCauses` com mutex (`ps2_memory.cpp:1940-1955`), drenada em `Store32`, no VBlank e em `sceDmaSend` | sem mudança (o fim é decidido no EE) |
| 39 | VIF1 i-bit (VIFcode com bit 31) | só liga `vif1_regs.stat` bit 11 (`ps2_vif1_interpreter.cpp:541`); **não** gera IRQ nem para o VIF | sem mudança (o parse fica no EE) |
| 40 | VU1 D/T-bit (parada) | grava `vu0_vpu_stat` bits 9/10 do contexto **corrente** depois da MSCAL (`ps2_runtime.cpp:739-742`); **nenhuma** IRQ INTC 7 | a thread VU1 publica `stoppedByD/T` numa palavra atômica; o EE aplica no próximo checkpoint/syscall (como `Get_MTVUChanges`). Diferença de tempo só visível às funções de depuração da libvu (#17) |
| 41 | GS SIGNAL/FINISH/HSYNC/VSYNC (INTC 0) | **não entregue** hoje (nenhum `dispatchIrq(false, 0)`) | futuro: a thread GS publica bits atômicos (como `mtvuInterrupts`); o escalonador do EE entrega no próximo ponto seguro (`processPendingEvents`). Sem mudança de comportamento na etapa 1 |

## 3. Arquitetura proposta

### 3.1 Threads e posse

| Thread | Possui (só ela escreve) | Lê de outras |
|---|---|---|
| **EE** | RDRAM, scratchpad, contexto R5900, escalonador, IOP host, DMAC e `m_ioRegisters`, estado do parser VIF1 (`vif1_regs` menos ROW, TOPS/DBF, UNPACK parcial, PATH2 IMAGE pendente), cópia espelho de MSKPATH3, VU0, `vsyncTick`, CSR (escritas do EE) | flags atômicas publicadas pela VU1/GS |
| **VU1** | `m_vu1Code`, `m_vu1Data`, `VU1Interpreter` (estado, pipeline, cache decodificado, `m_extension` do nativo), ROW/COL efetivos, geração do código | só R1 |
| **GS** | `GS` (frontend inteiro, `m_stateMutex` vira desnecessário), `GifArbiter`, `GifStreamState`, máscara PATH3 + fila de retidos, backend (CPU, `cpu-thread`, banded ou HW D3D11: o contexto D3D11 fica numa thread só), quadro apresentável | R2, R3 |
| **janela** | janela raylib, textura de apresentação, entrada do host | buffer triplo do quadro pronto, `vsyncTick` |

O rasterizador por CPU pode continuar atrás de `GSThreadedBackend` (uma 5ª thread) se for o gargalo. Com o backend HW,
o frontend e o backend dividem a thread GS.

### 3.2 Filas

| Anel | Produtor → consumidor | Conteúdo | Tamanho inicial | Por quê |
|---|---|---|---|---|
| R1 | EE → VU1 | `UNPACK{addr, vn, vl, cl, wl, num, mask, mode, flags, col?, payload}`, `WRITE_MICRO`, `WRITE_DATA`, `WRITE_ROW/COL`, `CALL{pc, top, itop, fbrst}`, `CONT{top, itop, fbrst}`, `VIF_RESET`, `FENCE{n}`, `QUIT` | 16 MiB | a VU1 roda ~3 000 chamadas por quadro; os UNPACKs de um quadro são poucos MB [NAO MEDIDO: contar bytes no passo 0] |
| R2 | EE → GS | `GIF{path, drainNow, bytes}`, `DRAIN`, `TICKET`, `MSKPATH3`, `VSYNC{tick, priv regs}`, `GS_RESET`, `FENCE{n}`, `CALL{fn}` (leituras), `QUIT` | 32 MiB (o valor que `gs-threading.md` mediu como suficiente) | rajadas de ~6,7 MB de comandos por quadro |
| R3 | VU1 → GS | `PATH1{bytes}` por XGKICK completo; `END_CALL` por chamada | 8 MiB | protótipo: ~1,2 MB de PATH1 a cada 6 000 chamadas (3 vsyncs) |

- **SPSC** de bytes com mensagens de tamanho variável e alinhadas a 16. Uma mensagem nunca dá a volta: um item `PAD`
  completa o fim do anel. As posições são contadores monotônicos atômicos (release/acquire). Uma mensagem maior que
  metade do anel é recusada: um pacote GIF gigante vai em pedaços, que a thread GS remonta, porque `GifStreamState`
  já mantém estado entre pacotes.
- **Espera do consumidor**: gira um pouco e depois dorme num evento. O produtor só acorda o consumidor se ele estiver
  dormindo (contador sob CAS), como `gs_command_queue.h`.
- **Contrapressão com histerese** (lição de `gs-threading.md`): o produtor bloqueado só acorda quando o anel cai a 50 %.
- **Limite de quadros adiantados**: o EE não passa mais de **2 vsyncs** à frente do último `VSYNC` que a thread GS
  terminou (como o MTGS limita a fila de vsync). Sem isso, um EE rápido enche 32 MiB de quadros que ninguém vê.
- **Publicação em lote**: o EE escreve vários itens e publica a posição de uma vez (no fim de cada transferência DMA,
  a cada N KB e **sempre antes de qualquer espera**).

### 3.3 Ordem dos pacotes GIF (o ponto central)

O resultado síncrono hoje é uma função da sequência de `submit(path, pacote)` e `drain()` no `GifArbiter`. O desenho
reproduz **essa sequência**, não um árbitro de hardware novo:

1. EE, ao executar a MSCAL k: publica `CALL k` em R1 e **depois** escreve `TICKET` em R2.
2. VU1, ao rodar a chamada k: cada XGKICK concluído vira `PATH1` em R3; no fim da chamada, `END_CALL`.
3. GS percorre R2 em ordem. `GIF`/`DRAIN`: faz no arbiter o que o EE fazia. `TICKET`: consome R3 até o `END_CALL`,
   fazendo `submit(Path1)+drain()` para cada `PATH1`, exatamente como `submitGifPacket(Path1, drain=true)` hoje.
   Os tickets saem na mesma ordem das chamadas (R1 e R3 são FIFOs), então o ticket não precisa de número.

Os pacotes PATH2 (DIRECT) que vêm depois de uma MSCAL no fluxo do VIF entram em R2 depois do ticket. Por isso, o GS
os processa depois dos PATH1 daquela chamada, como hoje. O protótipo valida essa mecânica com dados reais (seção 9).

Uma chamada que estoura o orçamento de 65 536 ciclos e é retomada por MSCNT gera dois tickets, um por chamada.
No hardware, PATH1/2/3 se intercalam em fronteiras de pacote, com prioridade. Fazer isso de verdade (como o PCSX2
faz) mudaria a saída e exigiria uma nova linha de base de regressão. Fica fora deste desenho.

### 3.4 Pontos de sincronização obrigatórios

Implementação: `WaitVU()` publica R1, espera R1 vazio **e** a VU ociosa (um contador `completedCalls` igual a
`issuedCalls`). `WaitGS()` publica R2, escreve `FENCE{n}` e espera `gsFence ≥ n`. `WaitAll()` faz `WaitVU()` e depois
`WaitGS()`: com a VU vazia, todos os PATH1 já estão em R3, e o fence de R2 vem depois dos tickets deles.

| Gatilho no EE | Espera | Item da seção 2 |
|---|---|---|
| leitura de memória da VU1 (código/dados) pelo EE ou DMA SPR | WV | 15, 16 |
| leitura de VIF1 ROW | WV | 14 |
| leitura de CSR / SIGLBLID; escrita em CSR | WA | 24, 25 |
| `sceGsExecStoreImage` / leitura de VRAM | WA (+ resposta) | 27, 32 |
| `sceGsResetGraph`, `ResetGraph` do frontend, VIF1/GS reset | WA | 13, 25, 28 |
| desligar o runtime, salvar estado | WA + QUIT | — |
| anel cheio, EE 2 vsyncs à frente | contrapressão | 3.2 |

Todo o resto é assíncrono: UNPACK, MPG, MSCAL/MSCNT, STROW/STCOL, PATH2/PATH3, MSKPATH3, VSYNC, fins de DMA,
FLUSH*, `sceGsSyncPath`, `sceDmaSync`, CHCR/D_STAT/GIF_STAT.

### 3.5 Por que isso é igual ao modo síncrono

- O EE só observa o resultado da VU1 e do GS pelos gatilhos da tabela acima, e todos eles esperam. Entre dois
  gatilhos, o EE vê exatamente o que via antes: DMA pronto, VBS1 = 0, fins de DMA instantâneos.
- A VU1 recebe as mesmas entradas, na mesma ordem: estado, código, dados depois dos UNPACKs, TOP/ITOP e DE/TE. É o
  mesmo objeto `VU1Interpreter`, que mantém o estado do pipeline entre chamadas. O protótipo confirma isso bit a bit.
- O GS recebe a mesma sequência de submits/drains (3.3) e os mesmos registradores de exibição a cada vsync.
- **Exceção prevista**: CSR SIGNAL/FINISH ficam visíveis depois. Quem lê, espera (WA), então lê o mesmo valor. Só muda
  quantas voltas um laço de espera dá, e isso não é observável pelo jogo.
- O arredondamento da VU (`fesetround(FE_TOWARDZERO)` em `run()`, `ps2_vu1_core.cpp:1623`) é por thread e já é
  definido a cada `run()`. FTZ não é ligado em nenhuma thread (`vu1-optimization.md`, riscos).

### 3.6 Modelo de tempo

- O relógio do guest fica **só** na thread EE: `m_eeCycle`, timers, VBlank, alarmes. A VU1 e o GS não somam ciclos
  ao EE (como hoje). Se um dia somarem (MTVU soma a média das últimas chamadas), a mudança é deliberada e quebra a
  equivalência.
- **Determinismo para testes**: hoje o VBlank exige ciclos **e** tempo do host (3.3 #33). Proposta:
  `PS2X_GUEST_CLOCK_ONLY=1` ignora o `hostDeadline` (vsync só por ciclos). Com isso o EE fica determinístico,
  independente da velocidade das threads, e as corridas `ee|vu|gs|all` são comparáveis por hash.
  [NAO VERIFICADO] se algum serviço do IOP host usa relógio de parede: o IOP pode ser outra fonte de não determinismo.
- **Pad virtual com relógio do guest**: hoje o script roda na thread da janela e lê `vsyncTick`. Com o EE mais rápido
  que a janela, um evento do script cai alguns vsyncs depois do previsto. Proposta: aplicar o script **na thread EE,
  dentro do VBlankStart** (`processEvent`, `EeScheduler.cpp:1939`), pelo `m_vsyncTick` exato. O `shot` vira um pedido
  `VSYNC{n, shot=nome}` em R2, que a thread GS atende com o quadro daquele vsync. Assim as capturas de regressão ficam
  determinísticas (hoje dependem de quando a janela acorda, `gs-threading.md` §3).

### 3.7 Interrupções vindas das threads auxiliares

Palavra atômica `auxIrqPending` (bits: VU1_T, VU1_D, GS_SIGNAL, GS_FINISH, GS_LABEL), com `fetch_or` (release) no
produtor e `exchange(0)` (acquire) no EE. Payloads (SIGLBLID, bits de VPU_STAT) ficam em atômicos ao lado. O EE
consulta a palavra em `processPendingEvents` (todo checkpoint já passa por lá) e em `waitForEvent`. Para ele não ficar
dormindo, a thread auxiliar chama `postEvent(ExternalWake)`. A entrega usa os caminhos que já existem
(`dispatchIrq`, `queueInvocation`), sempre na thread EE. Na etapa 1, nenhuma dessas IRQs é entregue (como hoje): só
os bits de VPU_STAT.

### 3.8 Regras contra deadlock

1. Antes de **qualquer** espera, o EE publica R1 e R2. O ticket só entra em R2 **depois** de o CALL estar publicado em
   R1 (senão o GS espera em R3 por uma chamada que a VU nunca viu).
2. A thread GS consome R3 **em fluxo** (pacote a pacote), não "a chamada inteira". Uma chamada com saída maior que
   R3 não trava.
3. A VU1 só espera vaga em R3, e o GS só espera dados (R2/R3). O EE espera vaga em R1/R2 ou fences. Não há ciclo:
   GS → VU (dados de R3) → EE (dados de R1, regra 1) → GS (vaga em R2) só fecharia se o GS esperasse vaga em algo,
   e ele não espera.
4. `WaitVU` não pode ser chamado de dentro da thread VU1 (callbacks), nem `WaitGS` da thread GS. Use asserts com o id
   da thread.
5. A janela nunca espera a thread GS com trava: ela pega o último quadro pronto ou repete o anterior.

## 4. Riscos de corrida e de comportamento

| Risco | Onde | Mitigação |
|---|---|---|
| EE escreve na memória da VU1 enquanto a VU roda | 2.1 #15/#16 | escritas viram comandos em R1; leituras esperam (WV) |
| UNPACK de um quadro sobre o buffer que a VU ainda lê | dupla bufferização TOPS/DBF | os UNPACKs entram em R1 depois do CALL anterior e antes do próximo: a VU os aplica na ordem do programa, como o modo síncrono (que não modela concorrência real). O jogo não pode depender de uma corrida de hardware que o modo síncrono já não tinha |
| ROW acumulado (MODE 1/2) divergir entre EE e VU | #4/#7 | ROW autoritativo na VU; leitura de ROW pelo EE é WV |
| `m_ioRegisters` é `std::map` sem trava | `ps2_memory.cpp` | continua só na thread EE. A VU1 e o GS **não** podem chamar `writeIORegister` (hoje `flushMaskedPath3Packets` não chama; manter assim) |
| `t_currentPath` (`thread_local`) do arbiter | `ps2_gif_arbiter.cpp:7` | arbiter e frontend na mesma thread (GS) |
| `GS::m_stateMutex` disputado pela janela | `gs_frontend.cpp:548` | a janela deixa de chamar o frontend (3.1, #31) |
| `siglblid` não atômico | `gs_frontend.cpp:1666-1668, 1690-1692` | só a thread GS escreve; o EE lê depois de WA |
| Painel de depuração lê estado de VU/GS ao vivo | `ps2_debug_panel.cpp` | já é racy hoje; com threads, ler por `CALL{fn}` em R2/R1 ou aceitar leitura "suja" só na depuração |
| VPU_STAT VBS1 lido inline | #17 | jogo-específico: seguro aqui. Genérico: o recompilador deveria emitir um hook para `cfc2 vi29` (regenerar, ~14 min de build) |
| Jogos que fazem polling de CSR.FINISH em laço apertado | #24 | WA a cada leitura custa uma ida e volta de thread. Contar no passo 0; se for quente, pré-varredura no produtor (como o `Gif_HandlerAD` do PCSX2) |
| `PS2X_GS_GIF_STATE=1` (parser com estado entre pacotes) | `gs-threading.md` §4 | a ordem é a mesma; continua opcional |
| `PS2X_GS_BANDS` | `gs_banded_backend.*` | independente (dentro do backend) |
| Saída de VU0 (`VCALLMS`) que lê dados da VU1 | #18 | [NAO VERIFICADO]; se existir, WV |
| Pré-requisito: VU1 nativo e GS HW mexem em `vu/` e `gs_hw_*` agora | — | o desenho só toca os pontos de chamada (`ps2_vif1_interpreter.cpp`, callbacks em `ps2_runtime.cpp`, `submitGifPacket`, `UploadFrame`). Nada dentro de `vu/` nem dos backends |

## 5. Comparação com o PCSX2 (GPL-3.0: só ideias, nenhum código copiado)

Lido em 05/11 (leitura de código-fonte público, sem executar nada): `pcsx2/MTVU.cpp`, `pcsx2/Memory.cpp` (handlers de
memória da VU) e `pcsx2/Gif_Unit.cpp`, em `raw.githubusercontent.com/PCSX2/pcsx2/master/`. O resto é de memória e está
marcado.

| Tema | PCSX2 | Aqui |
|---|---|---|
| Fila da VU1 | `VU_Thread`: anel SPSC de palavras, posições atômicas, `MTVU_NULL_PACKET` para dar a volta, margem de segurança no `WaitOnSize` | igual em espírito (R1), com mensagens de bytes alinhadas a 16 e PAD |
| Comandos | `MTVU_VU_EXECUTE` (addr, TOP, ITOP, FBRST), `MTVU_VU_WRITE_MICRO/DATA`, `MTVU_VU_WRITE_VIREGS/VFREGS`, `MTVU_VIF_WRITE_COL/ROW`, `MTVU_VIF_UNPACK` (estrutura do VIF + registradores + payload) | os mesmos papéis. VI/VF da VU1 não são graváveis pelo EE neste runtime |
| Leitura de memória da VU1 pelo EE | `vuMicroRead*/vuDataRead*` chamam `vu1Thread.WaitVU()`; escritas viram `WriteMicroMem/WriteDataMem` | igual (#15) |
| Espera geral | `WaitVU` via semáforo "vazio" | `WaitVU` por contador de chamadas |
| VPU_STAT / ciclos | soma ao EE a média dos últimos 4 `vuCycles` (limitada) e liga VBS1 com um evento `VU_MTVU_BUSY`, salvo `INSTANT_VU1` | não soma (equivale ao `INSTANT_VU1`); VBS1 = 0 |
| Interrupções | `mtvuInterrupts` (bits atômicos): SIGNAL, FINISH, LABEL, VU E-bit, VU T-bit (`hwIntcIrq(7)`); `Get_MTVUChanges` roda no EE | `auxIrqPending` (3.7) |
| SIGNAL/FINISH/LABEL | detectados **no produtor**: `Gif_HandlerAD` (EE) e `Gif_HandlerAD_MTVU` (VU, adia via atômicos). Um segundo SIGNAL com CSR.SIGNAL ligado fica pendente (stall) | etapa 1: WA na leitura; etapa 2 opcional: pré-varredura no produtor, como eles |
| PATH1 ↔ GS | a thread VU termina o pacote PATH1 (`FinishGSPacketMTVU`) e sinaliza o MTGS (`semaXGkick`); o arbiter do GIF intercala os caminhos como o hardware [ordem exata NAO VERIFICADA] | ordem reproduzida por tickets (3.3): mais simples e com a mesma saída do modo síncrono |
| MTGS | anel do GS em thread própria, `WaitGS` em leituras de VRAM, limite de vsyncs na fila [NAO VERIFICADO no fonte desta vez, de memória] | R2 + limite de 2 vsyncs (3.2) |
| local→host | `TRXDIR=1` calcula o tamanho do download no produtor (`GSLastDownloadSize`); a espera pelo MTGS fica em outro arquivo [NAO VERIFICADO] | WA (#27) |

Principal diferença: o PCSX2 emula o hardware (arbitragem real, ciclos de VU somados ao EE). Nós precisamos
**manter a saída idêntica ao modo síncrono** para usar a regressão por imagem como prova. Por isso, a ordem vem do
programa (tickets), não do tempo.

## 6. Plano de implementação (etapas pequenas, cada uma verificável)

Chave única: `PS2X_THREADS=off|gs|vu|all` (padrão `off` até a etapa 6 passar). `PS2X_THREADS_LOCKSTEP=1` faz o
produtor esperar o consumidor depois de cada item: a mecânica é a mesma, mas sem concorrência real, para isolar erros
de ordem dos de corrida.

| # | Etapa | Muda | Critério de corretude | Mede |
|---|---|---|---|---|
| 0 | **Contadores** (sem threads): quantas vezes por quadro o EE lê memória da VU1, VIF1 ROW, CSR/SIGLBLID, escreve CSR, local→host; bytes de UNPACK/MPG/PATH1/PATH2/PATH3 por quadro; MSCALs e DIRECTs por quadro | `ps2_memory.cpp` (contadores atômicos, saída no `[hb]`) | `regress.ps1 -Scenario scene3d` idêntico | frequência de cada gatilho WV/WA (decide se a pré-varredura do CSR é necessária) |
| 1 | **Relógio só do guest + pad no EE** (`PS2X_GUEST_CLOCK_ONLY`, pad no VBlankStart, `shot` no vsync exato) | `EeScheduler.cpp`, `ps2_runtime.cpp` | duas corridas seguidas em `off` dão o mesmo hash do fluxo GS por vsync (etapa 2) e as mesmas capturas | — |
| 2 | **Hash do fluxo GS** (`PS2X_GS_STREAM_HASH=1`): FNV de cada pacote que entra em `GS::processGIFPacket` (com o caminho), impresso a cada vsync | `gs_frontend.cpp` (só o hash) | estável entre corridas com a etapa 1 | é o **oráculo** das etapas seguintes |
| 3 | **Thread GS com frontend + arbiter** (`PS2X_THREADS=gs`): R2, `VSYNC{priv regs}`, apresentação por buffer triplo, WA para CSR/local→host | `ps2_memory.cpp` (`submitGifPacket` → R2), `gs_frontend` (sem a janela), `UploadFrame` | hash do fluxo por vsync **igual** ao de `off`, primeiro em lockstep e depois livre; `regress.ps1` menu + scene3d idênticos | ocupação da thread GS e esperas WA. Com o backend HW, a thread EE perde o rasterizador e o frontend |
| 4 | **Thread VU1 com fila de chamadas** (`PS2X_THREADS=vu`, exige 3): R1/R3, tickets, WV, ROW/COL na VU, bits D/T por flag | `ps2_vif1_interpreter.cpp` (UNPACK/MPG/MSCAL → R1), callbacks de `ps2_runtime.cpp:725-761`, `submitGifPacket(Path1)` → R3 | (a) replay: protótipo da seção 9 com a mesma interface; (b) `PS2X_THREADS_VERIFY=vu`: uma VU1 "sombra" síncrona, com memória própria, recebe os mesmos comandos na thread EE e compara, a cada `END_CALL`, o hash de estado, dados e PATH1 com o da thread VU1 (como o verificador de faixas do GS); (c) hash do fluxo GS igual ao de `off`; (d) regressão por imagem idêntica | ocupação da thread VU1, esperas do EE em R1, esperas do GS em R3 |
| 5 | **Interrupções auxiliares** (`auxIrqPending`), só os bits de VPU_STAT (sem IRQ nova) | `EeScheduler.cpp` | igual à etapa 4 | — |
| 6 | **Limite de 2 vsyncs, histerese, tamanhos de anel** | anéis | sem regressão; sem deadlock em 30 min de `scene3d` com `PS2X_THREADS=all` | vsync/s e latência de `Present` |
| 7 | (opcional) pré-varredura de A+D (SIGNAL/FINISH/LABEL) no produtor | parser GIF leve | hash igual; CSR igual ao de `off` nos pontos de leitura | só se a etapa 0 mostrar CSR quente |

Ordem com os outros agentes: as etapas 0-2 não dependem de VU nativo nem de GS HW e podem começar já. A 3 entra
depois do `gs_hw_*` estabilizar (ela muda quem chama o backend, não o backend). A 4 entra depois do VU nativo (o
protótipo já mostra que o nativo roda numa thread separada com saída idêntica).

## 7. Ganho esperado e como medir

Com a distribuição do perfil de 04/11 (350 ms/quadro numa thread), supondo o GS por HW e o VU1 ~3x mais rápido:
- **EE**: guest + runtime + IOP + VIF/GIF + frontend ≈ 10-12 % de 350 ms ≈ **35-45 ms** [estimativa: o frontend e o
  parser hoje somam 0,4 %, mas a cópia dos pacotes para os anéis é custo novo].
- **VU1**: ~110 ms / 3 ≈ **35-40 ms** (menos com o nativo).
- **GS HW**: [NAO MEDIDO na gameplay; `gs-gpu-measurements.md` traz os números parciais].
- Numa thread: soma ≈ 75-85 ms (~12-13 vsync/s). Com 3 threads: o mais lento ≈ 40 ms (~25 vsync/s). **~1,9-2x**
  sobre o mesmo código numa thread. O ganho vem de sobrepor EE e VU1, que hoje se alternam em série a ~3 000 chamadas
  por quadro. 50 Hz ainda exige reduzir o EE (IOP `beginNextReady`) e a VU1.

Como medir:
- `[threads]` a cada 10 s: ocupado/ocioso por thread, esperas do produtor por anel, WV/WA por motivo, bytes por anel.
- O amostrador (`ps2_sampling_profiler`) hoje só cobre a `GameThread`: estender para receber vários handles, com o
  nome da thread na saída. O mesmo `tools/resolve_profile.py` resolve.
- Corrida A/B em `scene3d` com `PS2X_GUEST_CLOCK_ONLY=1`: vsync/s em `off`, `gs`, `vu` e `all`, máquina sem outra carga
  (pedir o `MAQUINA.lock`).

## 8. Itens NAO VERIFICADOS / NAO MEDIDOS

- Se o jogo lê memória da VU1, VIF1 ROW ou CSR por MMIO, e quantas vezes (etapa 0).
- Se o interpretador da VU0 acessa registradores da VU1.
- Se o IOP host usa relógio de parede (afeta o determinismo da etapa 1).
- Bytes de UNPACK por quadro (tamanho de R1).
- Ordem exata de PATH1/2/3 no PCSX2 e onde o MTGS espera a VRAM (só de memória).
- O ganho real das etapas 3/4 (seção 7 é estimativa).

## 9. Protótipo isolado do MTVU (feito)

`work/threading_proto/mtvu_proto.cpp` + `build.bat` (fora do runtime e fora do git: `work/` não é versionado; se for
para versionar, copiar para `tests/threading_proto/` num commit do usuário). Compila com `cl.exe` direto e com as
mesmas flags do runtime contra os fontes **atuais** de `src/lib/vu/` (só leitura) e o `fake/` do `tests/vu1_replay`.
Log: `logs/threading_proto_build.log`, `logs/threading_proto_run.log`.

O que faz: lê as gravações `PS2X_VU1_RECORD` e roda em dois modos.
- `sync`: igual ao `vu1_replay` (uma thread).
- `async`: thread EE (VU0 inline; VU1 → `MPG`/`DATA`/`CALL` em R1, e o `TICKET` em R2 depois do CALL), thread VU1
  (dona do interpretador e das memórias; PATH1 de cada chamada em R3 + `END_CALL` com os hashes) e thread GS (percorre
  R2; no ticket, consome R3 até o fim). Os anéis são SPSC de bytes com PAD na volta, posições atômicas e espera ativa
  curta + `yield`. Os digests são escritos pela thread GS, na ordem em que ela recebe.

Resultados (05/11 ~01:00, máquina compartilhada):

| Gravação | Chamadas | sync == vu1_replay ref | async == sync | async (nativo) == ref | Pacotes PATH1 |
|---|---|---|---|---|---|
| `rec_gameplay.bin` (vsync 7560-7562) | 6 000 | sim | sim | sim | 2 134 (1,21 MB) |
| `rec_7563.bin` (vsync 7564-7568) | 9 000 | sim | sim (anel de 16 MiB **e** de 256 KiB) | sim | 3 155 (1,78 MB) |
| `synth4.bin` (sintético) | 8 000 | sim | sim | — | 0 |

- Cada modo rodou 3 vezes; o programa compara as passadas entre si (determinismo) e todas deram iguais.
- Tempo: async ≈ sync (0,18 s contra 0,20 s; 0,27 contra 0,31). O produtor não tem trabalho próprio aqui, então não há
  o que sobrepor. O número só mostra que o custo dos anéis (cópia de 16 KB de dados por chamada + 3 trocas de thread) fica
  escondido. O ganho real só aparece no runtime, quando o EE tem o guest para rodar enquanto a VU1 trabalha.
- R1 encheu milhares de vezes (o produtor só copia, então corre à frente até a contrapressão). R2 e R3 nunca bloquearam
  o produtor. Com o anel de 256 KiB, o consumidor de R2 esperou 276 vezes. A contrapressão foi exercitada sem deadlock.
- Limites do protótipo: o `DATA` manda a memória de dados inteira (no runtime seriam os UNPACKs); o estado da VU vem da
  gravação em cada chamada (no runtime ele persiste na thread VU1); não há PATH2/PATH3 nas gravações, então a
  intercalação com DIRECT só é coberta pelo raciocínio de 3.3; o "GS" só faz hash (não chama o frontend).

Como reproduzir (de `ghost_pc/`):
```
work\threading_proto\build.bat
work\threading_proto\out\mtvu_proto.exe work\vu1_bench\rec_gameplay.bin out_sync.txt sync 16384 3
work\threading_proto\out\mtvu_proto.exe work\vu1_bench\rec_gameplay.bin out_async.txt async 16384 3
fc /b out_sync.txt out_async.txt
```
(`PS2X_VU1_NATIVE=1` no ambiente usa o executor nativo.)

## 10. Fontes consultadas

- Código do runtime em `work/ps2recomp_game/ps2xRuntime` (base PS2Recomp, GPL-3.0), só leitura.
- PCSX2 (GPL-3.0), leitura em 05/11: `https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/MTVU.cpp`,
  `.../pcsx2/Memory.cpp`, `.../pcsx2/Gif_Unit.cpp`. Nada copiado; só os conceitos da seção 5.
