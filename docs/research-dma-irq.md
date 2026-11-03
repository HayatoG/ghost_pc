# Pesquisa: interrupcao de fim de DMA (DMAC) no PS2 e jogo preso em WaitSema

Data: 2026-11-03. Escrito de forma generica (pode virar `docs/` da raiz). Só links e resumos; nenhum código copiado (PS2Recomp e PCSX2 são GPL-3.0). Itens marcados (?) não foram confirmados em fonte primária nesta sessão.

Sintoma de referência: o jogo registra um handler com `AddDmacHandler` (causas 1=VIF1 e 2=GIF), dispara DMAs com `sceDmaSend` e fica em `WaitSema` esperando um `iSignalSema` que só o handler faz. Nenhum quadro desenhado.

## 1. Hardware: como o DMAC sinaliza o fim

Fontes: [ps2tek](https://psi-rockin.github.io/ps2tek/) (espelho: [israpps.github.io/ps2tek](https://israpps.github.io/ps2tek)), [ps2sdk kernel.h](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h), [NetBSD/playstation2 dmac.c](https://ftp.riken.jp/NetBSD/NetBSD-release-10/src/sys/arch/playstation2/ee/dmac.c).

### Registradores
| Registrador | Endereço | Campos relevantes |
|---|---|---|
| D_CTRL | 0x1000E000 | bit0 DMAE (habilita DMA), bit1 RELE, bits2-3 MFIFO, 4-7 stall |
| D_STAT | 0x1000E010 | bits0-9 status por canal (**escrever 1 limpa**); bit13 STAT stall, 14 MFIFO vazio, 15 BUSERR; bits16-25 **máscara** por canal (**escrever 1 inverte** o bit); bits29-30 máscaras de stall/MFIFO |
| D_PCR | 0x1000E020 | bits0-9 controle de CPCOND0: CPCOND0 = ((~PCR[0:9] \| D_STAT[0:9]) == 0x3FF); bits16-25 prioridade; bit31 PCE |
| INTC_STAT / INTC_MASK | 0x1000F000 / 0x1000F010 | W1C no STAT, W1-toggle na MASK |
| Dn_CHCR | 0x1000x000 | bit0 DIR, bits2-3 MOD (0 normal, 1 chain, 2 interleave), bit6 TTE, **bit7 TIE**, **bit8 STR** (start/busy), bits16-31 TAG |

Canais (e "causa" do `AddDmacHandler`, igual ao número do canal): 0 VIF0, **1 VIF1**, **2 GIF**, 3 IPU_FROM, 4 IPU_TO, 5 SIF0, 6 SIF1, 7 SIF2, 8 SPR_FROM, 9 SPR_TO; 13/14/15 = CIS/MEIS/BEIS (ps2sdk). Causas INTC: 0 GS, 1 SBUS, 2 VBLANK_S, 3 VBLANK_E, 4 VIF0, 5 VIF1, 6 VU0, 7 VU1, 8 IPU, 9-11 TIM0-2 (TIM3 reservado ao kernel), 13 SFIFO, 14 VU0WD.

### Regra de disparo (nível, não borda)
- Ao terminar a transferência o DMAC liga o bit do canal em D_STAT[0:9].
- A linha INT1 do COP0 (Cause bit 11, atende em `0x80000200`; Status bit 11 = máscara IM3) fica ativa **enquanto `(D_STAT.status & D_STAT.mask) != 0`**. A interrupção é por **nível**: se a máscara for habilitada depois do fim do DMA, a interrupção dispara naquele momento. O handler (kernel/SDK) precisa limpar o bit (W1C) para a linha cair.
- INTC (INT0, Cause bit 10): `(INTC_STAT & INTC_MASK) != 0`. O DMAC **não** passa pelo INTC; DMAC = INT1, INTC = INT0 (INT0 tem prioridade). O "bit 1 = DMAC" do enunciado **não** está certo: INTC bit 1 é SBUS (SIF); o DMAC chega pelo próprio pino INT1. (Confirmado em ps2tek e no cabeçalho do ps2sdk.)
- O kernel Sony traduz INT1 em chamadas aos handlers registrados por `AddDmacHandler(canal, fn, next)` (ordem por `next`), passando o canal como argumento, e só depois reabilita (`ExitHandler` = `sync; ei`, aviso do ps2sdk de bug do kernel EE).
- `EnableDmac(canal)`/`DisableDmac` no SDK mexem na máscara do D_STAT (bit 16+canal); `iEnableDmac` é a versão para contexto de interrupção.

### Chain, tag IRQ e TIE
- Tag DMA: bits0-15 QWC, 26-27 PCE, 28-30 ID, **bit31 IRQ**, 32-62 ADDR. Tags de fim: REFE (0) e END (7). `ref`/`refs`/`cnt`/`next`/`call`/`ret` continuam.
- Se `CHCR.TIE=1` e a tag tem IRQ=1, a cadeia **termina** depois de transferir o QWC dessa tag, como se fosse END. Se `TIE=0` o bit IRQ é ignorado.
- Fim da transferência (por END/REFE ou IRQ+TIE) => limpa STR, liga o bit do canal em D_STAT e (se mascara ligada) INT1.
- Ordem observada no PCSX2 (resumo): primeiro `CHCR.STR` é zerado, **depois** o bit de D_STAT é setado; o stall do DMA é liberado por último. Quem inspeciona CHCR dentro do handler vê o canal já parado.
- A interrupção do **VIF** (bit I no VIFcode, `VIF1_STAT.INT/VIS`) é outra coisa: vai pelo **INTC causa 5** (INT0), não pelo DMAC.

### Resumo de como o PCSX2 emula (sem código)
Fontes: [Hw.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Hw.cpp), [R5900.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/R5900.cpp), [Gif.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Gif.cpp), [Vif1_Dma.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Vif1_Dma.cpp). Nomes de função citados para localizar a leitura, não copiar.
- `hwDmacIrq(canal)` seta o bit em D_STAT e, se o bit de máscara correspondente estiver ligado, chama `cpuTestDMACInts()`. `hwIntcIrq(n)` faz o análogo no INTC.
- `cpuTestDMACInts/cpuTestINTCInts` só agem se `cpuIntsEnabled`: Status.IM da linha ligado, **EIE e IE ligados, EXL e ERL desligados**. Se houver pendência, agendam o event test dentro de ~4 ciclos; a entrega real é em `_cpuEventTest_Shared` via `cpuException` (vetor 0x200 para interrupção; seta EXL, salva EPC). Enquanto EXL=1 não há nova interrupção.
- GIF: em `gifInterrupt()`, quando QWC=0 e o path3 terminou (`gspath3done`, que o TIE+IRQ da tag também ativa), zera STR e chama `hwDmacIrq(GIF)`. Se há SIGNAL do GS pendente, a DMA fica parada/reagendada (isso importa: SIGNAL/FINISH podem atrasar o fim do DMA).
- VIF1: em `vif1Interrupt()`, no fim zera STR, limpa stall e chama `hwDmacIrq(VIF1)`. Com `TIE`+IRQ da tag, `vif1SetupTransfer` marca `done`. Se o VIFcode tem I-bit, o PCSX2 gera INTC 5 e pode **parar** o VIF1 (stall) até `FBRST.STC`; nesse caso o DMAC **não** completa até soltar.
- Tudo é por evento agendado com latência (não síncrono dentro da escrita no CHCR). Um jogo que arma o handler logo após o `sceDmaSend` depende disso.

### GS (outros "fins" que acordam semáforos)
- INTC bit 0 = GS. GS_CSR: bit0 SIGNAL, bit1 FINISH, bit2 HSINT, bit3 VSINT, bit4 EDWINT (layout de memória/uso comum; ps2tek tem a página do GS, e um header do MAME confirma SIGNAL=1, FINISH=2: [ps2gs.h](https://git.redump.net/mame/tree/src/devices/video/ps2gs.h)). GS_IMR (bits 8-12) mascara os eventos. FINISH ocorre quando o GS termina o desenho e a escrita no registrador FINISH foi processada; o jogo costuma `sceGsSyncPath`/esperar FINISH. Se o port nunca liga CSR.FINISH nem INTC bit 0, um jogo que dependa disso trava do mesmo jeito (hipótese H8, e não é o nosso caso atual).
- `SetVSyncFlag` (syscall 0x73) copia GS_CSR a cada vsync; vsync = INTC 2/3.

## 2. Outros projetos

### PS2Recomp (ran-j) – runtime `ps2xRuntime`
- [PR #151](https://github.com/ran-j/PS2Recomp/pull/151) (aberto, mudanças pedidas) descreve **exatamente a nossa classe de travamento** (observado em Dragon Quest VIII): `submitDmaSend` termina a VIF1 de forma síncrona mas não levanta a causa; a sequência da libdma é `sceDmaSend` -> `AddIntcHandler/EnableIntc` -> `WaitSema`, então um dispatch no ponto de levantamento chega antes do handler existir e se perde. Solução proposta: **latch de pendência** (bit só limpa depois que um handler registrado e habilitado rodou, ou expira em ~2 s), drenado a cada tick; o levantamento passou a ser por borda real (I-bit do VIFcode) e não incondicional, porque levantar sempre dispara o handler a cada pacote e adianta semáforos.
- [PR #295](https://github.com/ran-j/PS2Recomp/pull/295) (aberto): I-bit levanta INTC 5, para o VIF1 até `FBRST.STC`, e a entrega de causas enfileiradas passa a ocorrer em `drainCompletedDmacHandlers` após o stub de `sceDmaSend` **e em todo VBlank** (antes só após um store de 32 bits); cita um deadlock em que a interrupção levantada dentro de `sceDmaSend` nunca era entregue porque o jogo ficava em laço só de loads. Cita também PR #343 (estender entrega a stores de 8/16/64/128 bits) e #341.
- [Wiki Ps2xRuntime](https://github.com/ran-j/PS2Recomp/wiki/Ps2xRuntime): runtime é a parte menos madura; DMA/VIF/GS "podem faltar ou ser simplificados"; breakpoints úteis `lookupFunction`, `TODO_NAMED`, `ps2_syscalls::TODO`; aviso "PC not updating" = laço/espera.
- [Walkthrough para jogos stripped](https://github.com/ran-j/PS2Recomp/wiki/PS2Recomp-Stripped-Game-Walkthrough-For-LLMs): ordem de correções: estabilidade de boot (funções registradas, syscalls TODO), depois I/O e módulos (`sceCd*`, `fio*`, `Sif*`), depois menu; stubs `ret0/ret1/reta0` só para classificar. Release [v0.4](https://newreleases.io/project/github/ran-j/PS2Recomp/release/v0.4): adicionou helpers de DMA e handlers de RPC; avisa que sem emular módulos IOP o jogo trava.
- **Game overrides** (wiki *Game Override Hooks*; resumo do walkthrough): código C++ em `ps2xRuntime/src/runner/` (ou `src/lib/` com CMake) registrado com macro `PS2_REGISTER_GAME_OVERRIDE(nome, serial, entry, crc, fn)`, chaveado por **basename do ELF + entry point opcional + CRC32 opcional**; dentro dele `runtime.registerFunction(addr, lambda)` substitui uma função recompilada por endereço, e há `bindAddressHandler(runtime, addr, "ret0")`. Cuidado: handler cru que não avança `ctx->pc` gera redispatch infinito no mesmo endereço (envolver em lambda que faz `pc = ra`). CRC/entry errados fazem o override **não aplicar em silêncio**. Quando servem: hack por título (substituir uma rotina de espera/sync, pular checagem de hardware, stubar função cujo SDK não existe). Para nós: um override poderia, por exemplo, substituir o `sceDmaSend`/handler do jogo, mas é remendo; a correção certa é o runtime emular o fim de DMA.
- Fluxo de patches por jogo: [Tomba2Recomp](https://github.com/mstan/Tomba2Recomp) (é PS1, só para o padrão) e [dmc-recomp](https://github.com/Gui-Tora/dmc-recomp) (PS2Recomp pinado + `patches/`, ciclo reproduzir -> isolar a primeira divergência -> comparar com PCSX2). Lições do dmc-recomp: o agendador do EE invertia a ordem de invocações enfileiradas (FIFO drenada como LIFO), quebrando callbacks/IRQ/VBlank; HLE de serviços SIF/CD; a cadeia VIF1 só chegava inteira ao GS depois de corrigir o caminhar da cadeia.
- Outros: [burnout3-pc](https://github.com/danielgomesvieira2000/burnout3-pc) (engine em estágios; docs em `games/burnout3/docs/` e `docs/PORTING.md` do engine; a página principal não detalha DMA/IRQ, não verificado); [ico-pc](https://github.com/nathanialf/ico-pc) (roda código decompilado do jogo sobre camada de compatibilidade; sem detalhes de IRQ no README). Nenhum issue público do PS2Recomp sobre "WaitSema/AddDmacHandler" foi achado pela busca; o PR #151/#295 é a melhor referência.

## 3. Problemas conhecidos parecidos e soluções
| Problema | Solução usual | Fonte |
|---|---|---|
| Fim de DMA nunca levanta interrupção (VIF1/GIF) -> WaitSema eterno | Levantar a causa no fim da transferência; guardar pendência até haver handler **registrado e habilitado**; modelar nível (D_STAT status&mask) | PR #151 |
| Handler registrado depois do kick (libdma: `sceDmaSend` -> `AddIntcHandler/Dmac` -> `WaitSema`) | Latch/nível em vez de evento perdido; ou adiar a conclusão do DMA (como o PCSX2, por evento agendado) | PR #151; PCSX2 `gifInterrupt/vif1Interrupt` |
| Entrega só depois de um store 32 bits; laço só de loads | Drenar também no VBlank, após `sceDmaSend`, em stores de todos os tamanhos, e no `WaitSema`/idle | PR #295, #343 |
| I-bit do VIFcode (INTC 5) não gerado, ou DMA não completa por stall | Gerar INTC 5, parar VIF1 até `FBRST.STC`, completar DMAC só depois | PR #295; PCSX2 `vif1Interrupt` |
| Ordem de handlers/invocações enfileiradas invertida | Fila FIFO, handlers por `next` | dmc-recomp |
| SIGNAL/FINISH do GS não gerados | CSR.SIGNAL/FINISH + IMR + INTC 0; o PCSX2 pausa o path3 em SIGNAL pendente | PCSX2 `Gif.cpp`; ps2tek |
| Timer/outro estado limpo pelo handler de DMA (relato de port Harry Potter) | Lógica do jogo; só para saber que handlers de DMA podem mexer em flags de timer ([Patreon](https://www.patreon.com/posts/39218662), não lido: 403) | (?) |

## 4. Hipóteses priorizadas para o nosso runtime

Observações do código local (`work/ps2recomp_game/ps2xRuntime`, lido nesta sessão):
- Fim de DMA: `PS2Memory::processPendingTransfers` seta `D_STAT` (`raiseDStatChannel`, ps2_memory.cpp ~1870) e enfileira a causa (`queueCompletedDmacCause`) para GIF(2)/VIF0/VIF1(1); GIF chain nativo também enfileira 2 (linhas ~2192, ~2272).
- A causa só vira interrupção em `drainCompletedDmacHandlers`, chamado **somente** após `Store32` (ps2_runtime.cpp ~2181), em `kickGifDmaChainFromMMIO` e no stub `sceDmaSend` (Support.h ~1380-1435). `Store64/Store128` **não** drenam; sem drenagem no vblank nem no idle.
- `EeScheduler::dispatchIrq` (EeScheduler.cpp ~1270): descarta se a causa não está em `m_enabledDmacMask` (sem latch; contador `maskedOut`), ignora handlers com `!enabled`, `cause` diferente ou `!hasFunction` (contador `handlerNotRecompiled`), e enfileira `GuestInvocation` com `a0=causa`, `a1=arg`, `ra=0` (a invocação termina quando o PC chega a 0).
- `PS2X_HEARTBEAT` já imprime as estatísticas (`ps2_irq_stats.h`: registered, dispatchCalls, maskedOut, handlerNotRecompiled, queued, enabledMask, lastRegisteredCause/Handler).

| # | Hipótese | Como confirmar por log |
|---|---|---|
| H1 | **Causa levantada antes de `AddDmacHandler`/`EnableDmac` e perdida** (sem latch; mesmo caso do PR #151). Os 6 DMAs já completaram e o `maskedOut`/`dispatchCalls` foi gasto antes do handler existir. | Heartbeat: `registered` e `enabledMask` x `dispatchCalls`/`maskedOut`. Logar com timestamp/ordem: cada `queueCompletedDmacCause`, cada `AddDmacHandler(cause,fn)`, `EnableDmac(cause)`, cada `dispatchIrq`. Se o último `dispatchIrq` ocorre antes do último `AddDmacHandler` ou `maskedOut>0`, confirmado. Correção: latch por causa (ou semântica de nível: reentregar quando `EnableDmac` ligar a máscara com D_STAT já setado). |
| H2 | **Causa ficou enfileirada e nunca drenada** (`drainCompletedDmacHandlers` só roda em `Store32`/stub; a thread principal está bloqueada em WaitSema, sem stores; escrita via `sd`/`sq`, ou conclusão adiada por outro caminho). | Logar o tamanho de `m_completedDmacCauses` no heartbeat (ou `consumeCompletedDmacCauses` no loop idle). Se for >0 e parado, confirmado. Correção: drenar também no `EeScheduler::run` (idle/waitForEvent), no vblank e em `Store8/16/64/128`. |
| H3 | **O handler é enfileirado mas nunca roda** (`queueInvocation` ok, mas o escalonador não o agenda enquanto a thread principal está em wait, ou `ra=0`/stack). | Heartbeat `queued>0` com `lastRegisteredHandler` sem efeito; logar entrada/saída da invocação `Interrupt` e se `iSignalSema` foi chamada. Se `queued>0` e nenhum `iSignalSema`, o problema é em `run()`/`acquireInvocationThread`. |
| H4 | **`handlerNotRecompiled`**: o ponteiro do handler é alvo apenas apontado (sem função gerada) ou aponta para endereço diferente (ex.: com bit de kseg/offset de thunk). | `handlerNotRecompiled>0` e `lastUnrecompiledHandler`; `PS2X_MISSING_LOG` já registra o alvo (`DmacHandler`). Criar a função no Ghidra (`CreateFunctionsAt.java`) e regerar. |
| H5 | **Máscara/causa desencontradas**: o jogo habilita outro número (ex.: `EnableDmac(1)` vs registra cause 2; ou usa DMAC_CIS 13) ou chama `EnableDmac` com o canal errado em `a0`; ou registra com `next` mudando a ordem. | Logar os argumentos de `AddDmacHandler`, `EnableDmac`, `DisableDmac` e `enabledMask` ao longo do tempo; comparar com as causas enfileiradas (1 e 2). |
| H6 | **D_STAT/D_PCR visto pelo jogo difere do hardware**: o handler/driver lê D_STAT (status&mask) antes de agir. O runtime seta bit 31 e não reflete máscara em `EnableDmac` (o kernel real toca em D_STAT.mask); `sceDmaSync` pode usar CPCOND0 (`D_PCR`) e `bc0t/bc0f` (não verificado se o runtime modela CPCOND0). | Trace de leitura/escrita em 0x1000E010/E020 (`ps2TraceGuestWrite` já existe; ligar o trace de leitura). Verificar se o jogo testa bits 16-25 e se `bc0f` é traduzido. |
| H7 | **Semáforo errado ou contagem**: o handler sinaliza outro id/contador (inicial/máx), ou o `WaitSema` espera um semáforo que nunca é o do handler (duas threads criadas, cada uma com o seu). | Logar `CreateSema` (id, init, max), `WaitSema`/`SignalSema`/`iSignalSema` (id, contagem, thread). Cruzar com o id usado pelo handler (pelo código do handler no Ghidra). |
| H8 | **Dependência de outro evento**: o fim real (após o último pacote) pode vir de GS FINISH/SIGNAL (INTC 0, `GS_CSR`) ou do I-bit VIF1 (INTC 5), não do DMAC; a fila só termina após o `FINISH` do GS. | Ver no disassembly do handler/espera se há leitura de `GS_CSR` (0x12001000), `GS_IMR` ou `AddIntcHandler(0/5)`; logar `AddIntcHandler` e `EnableIntc`; checar `gsw=0` do heartbeat (nenhum write ao GS). |
| H9 | **Interrupções mascaradas no jogo (DI/EI, Status.EIE/IE)**: o jogo faz `DI` ao redor do kick e o runtime ignora/inverte; ou entrega acontece dentro de região "DI" e adia indefinidamente. O runtime aparentemente não modela EIE (só `m_insideInterrupt`), então o inverso, entregar quando não devia, é mais provável do que bloquear. | Logar `DI/EI` (syscalls 0x?? e instruções COP0) e o instante de entrega. Menos provável como causa do travamento. |
| H10 | **Ordem/atomicidade: a cadeia depende de concluir DMA só depois do `WaitSema`** (conclusão síncrona no `CHCR` write faz o handler rodar antes de o jogo marcar "pendente", e o handler vê estado antigo, "limpa um bit" que ainda não foi setado e não envia o próximo pacote). | Logar a sequência (kick -> handler -> WaitSema) e o estado da variável de fila do jogo. Correção: atrasar a conclusão/entrega para o próximo tick do escalonador (como PCSX2, evento agendado) em vez de síncrono dentro do `CHCR`. |

Ordem sugerida de teste: H1 e H2 (um log de ordem e do tamanho da fila resolvem os dois), depois H3/H4 (contadores já existentes), H7, H10, H5/H6, H8.

### Correção estrutural recomendada (candidata)
1. Modelar a interrupção do DMAC como **nível**: manter `D_STAT.status` por canal como fonte de verdade, e entregar quando `(status & mask)` != 0 e houver handler habilitado; reavaliar em `EnableDmac`, `AddDmacHandler`, `EnableDmacHandler` e ao W1C do D_STAT.
2. Concluir DMAs por evento agendado (próximo ponto seguro do escalonador), não dentro da escrita no CHCR.
3. Drenar entregas em todo store de MMIO, no `sceDmaSend`, no vblank e no loop idle do escalonador.
4. Só depois, se necessário, um *game override* específico; ele é remendo.

## Fontes
- ps2tek: https://psi-rockin.github.io/ps2tek/ , https://israpps.github.io/ps2tek , https://israpps.github.io/ps2tek/PS2/DMAC/IO.html
- ps2sdk kernel.h: https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h
- NetBSD/playstation2 (driver real do DMAC): https://ftp.riken.jp/NetBSD/NetBSD-release-10/src/sys/arch/playstation2/ee/dmac.c , https://src.rcs.uwaterloo.ca/xref/netbsd-src/sys/arch/playstation2/ee/dmacreg.h
- PCSX2: https://github.com/PCSX2/pcsx2 (Hw.cpp, R5900.cpp, Gif.cpp, Vif1_Dma.cpp)
- PS2Recomp: https://github.com/ran-j/PS2Recomp , PR #151, PR #295, wiki Ps2xRuntime e Walkthrough (links acima)
- dmc-recomp: https://github.com/Gui-Tora/dmc-recomp ; burnout3-pc: https://github.com/danielgomesvieira2000/burnout3-pc
- MAME ps2gs.h (CSR): https://git.redump.net/mame/tree/src/devices/video/ps2gs.h
