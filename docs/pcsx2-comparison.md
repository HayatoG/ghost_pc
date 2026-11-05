# Comparação com o PCSX2: MTVU, microVU, GS por hardware e threads (05/11/2026, ~06:30)

Pesquisa só de leitura, feita durante uma medição de desempenho (nada compilado nem executado). O PCSX2 é GPL-3.0, como
o PS2Recomp: aqui há **só ideias**, com arquivo e URL; nenhum trecho além de 1 linha foi reproduzido. As URLs são do
branch `master` em `https://raw.githubusercontent.com/PCSX2/pcsx2/master/` (lidas em 05/11/2026). O que veio de memória,
sem leitura do arquivo nesta sessão, está marcado **[NAO VERIFICADO]**.

O nosso estado vem de `threading-design.md` (10.7), `vu1-native.md`, `gs-hw-backend.md`,
`gameplay-profile-hw2-2026-11-05.md` e `relatorio-performance.md`. Perfil da thread do jogo usado nas estimativas:
VU1 23 %, guest+VU0 17 %, runtime do EE 11,5 %, espera ociosa 10 %. Hoje ~23 vsync/s na cena 3D; alvo 50.

---

## 1. MTVU (VU1 em thread própria)

### 1.1 O que o PCSX2 faz (lido: `pcsx2/MTVU.cpp`)

- **Anel**: buffer de `u32`, um produtor (EE) e um consumidor (thread VU1). Comandos: `MTVU_VU_EXECUTE`
  (5 palavras: tag, endereço, TOP, ITOP, FBRST), `MTVU_VU_WRITE_MICRO` (chama `CpuVU1->Clear` antes da cópia),
  `MTVU_VU_WRITE_DATA`, `MTVU_VU_WRITE_VIREGS/VFREGS`, `MTVU_VIF_WRITE_COL/ROW`, `MTVU_VIF_UNPACK` (tag do VIF +
  registradores cycle/mode/num/mask/top/itop + tamanho + payload; o UNPACK roda **na thread VU**) e
  `MTVU_NULL_PACKET` (marca de volta do anel). É o mesmo conjunto do nosso anel (UNPACK/MPG/CALL/ROW).
- **Espera do EE**: só em `ReserveSpace` → `WaitOnSize`, que **gira** em `GetReadPos()` com `KickStart()` +
  `std::this_thread::yield()`, sem dormir, e deixa 4 KB de folga (há um FIXME sobre um bug de ponteiro). `WaitVU()`
  (`semaEvent.WaitForEmpty()`) só é chamada de `CommitWritePos` com `MTVU_SYNC_MODE`, que é `0`. **Na operação normal
  o EE não espera a VU1 nunca**, nem no vsync, nem no XGKICK, nem na leitura de registradores.
- **Acordar**: cada `Write*` faz `CommitWritePos` (store release) e `KickStart()` = `semaEvent.NotifyOfWork()`. O
  consumidor esvazia até `read == write` e volta a `WaitForWork()`. A política de giro antes de dormir fica no
  semáforo (`common/Semaphore.cpp`, `WorkSema`) **[NAO VERIFICADO: lembro de um giro curto antes do futex/evento]**.
- **Tempo emulado**: a thread VU grava `VU1.cycle` num anel de 4 (`vuCycles[4]`, release). `Get_vuCycles()` devolve a
  média (soma >> 2), "vu cycle stealing hack". `ExecuteVU` faz `cycles = max(Get_vuCycles(), 4)`, limita o salto a
  3000 e soma `skip_cycles * EECycleSkip` em `cpuRegs.cycle`. Sem `INSTANT_VU1`, liga o bit `0x100` (VBS1) em
  `VPU_STAT` e agenda `CPU_INT(VU_MTVU_BUSY, cycles)`: **o EE vê a VU1 ocupada por uma estimativa de ciclos, sem
  esperar a thread**.
- **Interrupções/efeitos para o EE**: `Get_MTVUChanges()` lê bits atômicos (`mtvuInterrupts`): SIGNAL/FINISH/LABEL
  (vindos de `Gif_HandlerAD_MTVU` no `Gif_Unit.cpp`), E-bit e T-bit (T → `hwIntcIrq(7)`). É chamada no começo e no
  fim de `ExecuteVU`: os efeitos chegam ao EE **na próxima chamada**, de forma atrasada e assíncrona.
- **PATH1**: depois de `Execute`, a thread VU faz `gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU()` e
  `semaXGkick.Post()`. No EE, logo depois de publicar o EXECUTE, `ExecuteVU` chama
  `gifUnit.TransferGSPacketData(GIF_TRANS_MTVU, NULL, 0)`: **quem empurra o PATH1 para o anel do MTGS é o EE**, em
  ordem com PATH2/PATH3; a thread VU só produz os pacotes (fila `gsPackQueue` no `Gif_Unit.h`
  **[NAO VERIFICADO: nome exato e onde o EE espera o pacote ficar pronto]**). O GS (MTGS) é outra thread.
- `Gif_Unit.cpp` lido: fora do save state (`vu1Thread.IsDone()` + `MTGS::WaitGS()` em `gifFreeze`) não há espera do EE
  pela VU nas transferências normais.

### 1.2 O que fazemos (etapa 4, `PS2X_THREADS=vu`)

Um anel SPSC de bytes (itens de 32 B + payload), consumidor gira 4 000 `yield` e dorme em `cv.wait_for(500 µs)`,
acorda só quando o atraso passa de 64 KB ou o EE vai esperar; EE espera o anel esvaziar (`waitIdle`) em 18 métodos do
frontend do GS, registradores privilegiados, memória da VU1, `empty()` do árbitro e **VBlankStart** (o único que
dispara na prática). O árbitro do GIF e o **frontend do GS rodam na thread VU1**. Resultado: correto, ~5,9 vsync/s
contra ~27-32 (off), mesmo número em 3 configurações; EE 91 % das amostras fora do exe (espera de kernel); `waitIdle`
~0 ms e anel cheio 2 ms pelos contadores.

### 1.3 Diferenças e hipóteses para a espera de ~168 ms por vsync (ordenadas)

1. **`cv.wait_for(500 µs)` no Windows vira ~1-15,6 ms** (resolução do timer do sistema; só é ~1 ms com
   `timeBeginPeriod(1)` ou timer de alta resolução). Com o acordar só por atraso > 64 KB ou por espera do EE, a VU1
   processa "aos solavancos". Se o EE estiver de fato parado em algum ponto que **não soma em `waitIdleNs`/`waitFullNs`**
   (ex.: `beginPush` na volta/PAD, ou um `waitIdle` chamado por um caminho sem contador), cada parada custa um quantum
   de timer: 168 ms ≈ 11 × 15,6 ms. PCSX2: o produtor acorda o consumidor **a cada escrita** (`KickStart` em todo
   `Write*`) e o consumidor espera num semáforo, não num `wait_for` com prazo. Teste barato: `timeBeginPeriod(1)` no
   início do runtime, ou trocar o `wait_for` por evento sem prazo + notify quando o EE publica e o consumidor dorme.
2. **`yield` (= `SwitchToThread`) com a máquina sobrecarregada** (2-4 runners de outros agentes): cada `yield` que
   encontra outra thread pronta no mesmo núcleo cede o resto do quantum (~15-30 ms). Três threads nossas girando (EE em
   `waitIdle`/anel cheio, VU1 com 4 000 `yield`, GS) disputam com os outros processos. A taxa idêntica em 3 configs
   bate com um custo fixo de escalonamento do SO, não com CPU. PCSX2 também gira com `yield` em `WaitOnSize`, mas
   **quase nunca chega lá** (anel grande, sem espera no vsync). Teste: medir com a máquina livre (`MAQUINA.lock`) e
   contar `yield` por vsync em todos os laços (não só tempo).
3. **Espera no VBlankStart (sincronismo por quadro que o PCSX2 não tem)**. O PCSX2 limita o EE à frente pelo
   **MTGS** (fila de quadros, `VsyncQueueSize`, padrão 2) **[NAO VERIFICADO: nome/valor]**, nunca pela VU. A nossa
   espera faz o EE esperar a VU1 **e** tudo o que ela chama: árbitro + frontend do GS + (com `hw`/`cpu`) o backend; com
   `hw-thread`, o frontend empurra para a fila do GS, cujo `Present`/leituras "esperam a fila esvaziar até eles". Se o
   contador de `waitIdle` mede só o laço e não o tempo em que o EE fica bloqueado **dentro** de uma chamada do
   frontend (mutex do GS/fila), a espera real fica invisível. Teste: cronometrar o VBlankStart inteiro, de ponta a ponta.
4. **Frontend do GS na thread VU1** (desvio do desenho). No PCSX2 a thread VU só executa microcódigo e gera pacotes
   PATH1; quem ordena PATH1/2/3 e alimenta o GS é o EE (`TransferGSPacketData(GIF_TRANS_MTVU)`), e o GS tem a sua
   thread. Conosco a thread VU1 acumula VU1 + árbitro + frontend + possível espera da fila do GS: se essa fila estiver
   cheia ou em `Present`, a VU1 dorme no kernel e o EE, ao chegar no VBlankStart, espera por tabela.
5. **Tempo emulado**: o PCSX2 avança o relógio do EE pelos ciclos estimados da VU1 (`Get_vuCycles`, `VU_MTVU_BUSY`) e
   sinaliza VBS1. Nós não devolvemos ciclos (igual ao serial). Não explica a lentidão do host (em guest-clock-only o
   relógio não tem pacing), mas, se o jogo fizer polling de `VPU_STAT`/`VIF1_STAT`/`FBRST` esperando a VU1, cada leitura
   nossa que força `waitIdle` e não é contada vira o ponto (1)/(3). Conferir se toda leitura de VPU_STAT/VIF1_STAT passa
   pelo contador.
6. **Medição**: o amostrador marcou 91 % "fora do exe" na thread do jogo; vale confirmar com ETW/WPR (pilha em
   `ntdll!NtWaitForAlertByThreadId`/`NtYieldExecution`/`ZwDelayExecution`) qual primitiva é. `NtYieldExecution` →
   hipótese 2; `NtWaitForAlertByThreadId`/SRW → mutex/condvar (1, 3, 4); `ZwDelayExecution` → `sleep_for`
   (`ps2_runtime.cpp:1597`, pacing).

**O que copiar do desenho do PCSX2 (ideia)**: (a) nenhuma espera do EE pela VU no vsync; o limite de avanço fica no GS;
(b) acordar por escrita, consumidor em evento sem prazo; (c) PATH1 produzido na thread VU mas **ordenado e entregue
pelo EE** (ou por uma fila VU→GS com tickets, como o nosso desenho 3.3 original); (d) efeitos para o EE (SIGNAL/FINISH/
LABEL/T-bit) por bits atômicos lidos na próxima chamada.

---

## 2. microVU (custo por instrução e recarga de microcódigo)

### 2.1 O que o PCSX2 faz

- **Cache por programa** (lido: `pcsx2/x86/microVU.cpp`): `microProgram` guarda uma cópia do microcódigo (`data`), as
  faixas usadas (`ranges`) e os blocos compilados por PC. Listas por PC de entrada (`prog.prog[startPC/8]`, deque) e um
  atalho `prog.quick[startPC/8]` com o último programa+bloco usado. `mVUsearchProg`: se `quick` é válido, entra sem
  comparar nada; senão percorre a lista comparando (`mVUcmpProg`: `memcmp` só das faixas usadas, ou do programa
  inteiro com `doWholeProgCompare`), move o achado para a frente e só compila se nada bater.
- **Escrita de microcódigo** (`mVUclear`, chamado do `WRITE_MICRO`/MPG): **não apaga nada**; marca `cleared = 1` e
  zera os `quick`. A próxima execução refaz a busca por comparação. Ou seja: **MPG com bytes idênticos = um `memcmp`
  das faixas usadas, zero recompilação**. Programas só são apagados no reset.
- **Flags preguiçosas** (lido: `pcsx2/x86/microVU_Flags.inl`): `mVUsetFlags` varre o bloco **de trás para frente** e
  marca só as últimas escritas de status/MAC/clip que alguém lê; o resto não é calculado (`sFlagCond`, `__Mac`,
  `__Clip`). Quatro instâncias de flag com atraso de 4 ciclos (`findFlagInst`); `mVUsetFlagInfo`/`_mVUflagPass` olham
  as ~4 primeiras instruções do bloco sucessor para decidir se precisam de valor exato; `mVUsetupFlags` materializa
  só no fim do bloco.
- **Alocação de registradores** **[NAO VERIFICADO nesta sessão]**: `microVU_IR.h` (`microRegAlloc`) mantém VF em xmm
  entre instruções do bloco, grava de volta só o que foi escrito (`writeBackReg` com máscara xyzw) e no fim do bloco.
- **Stalls/latências** **[NAO VERIFICADO]**: `microVU_Analyze.inl` (`mVUanalyzeFMAC*`, `analyzeReg*`) calcula os stalls
  estaticamente na compilação (por bloco e estado de pipeline de entrada `microBlockManager` por `pState`), sem
  contabilidade em tempo de execução.

### 2.2 O que fazemos

Executor nativo (laço por par upper/lower, ~30 ns/par, ~216 pares e ~6 µs por chamada, 700-2000 chamadas por vsync),
cache por **geração de bloco**: ~340 MPGs por quadro com bytes quase sempre idênticos incrementam a geração e
invalidam/recompilam (ou revalidam) o código nativo.

### 2.3 Ideias aplicáveis

- **Cache por conteúdo, não por geração** (como `mVUclear` + `mVUcmpProg`): no MPG, comparar os bytes novos com os que
  já estão na micromemória **antes** de marcar modificado; se iguais, não incrementar a geração. Mais barato ainda:
  hash (xxh3/CRC) das faixas usadas por programa e lista de programas por PC de entrada com atalho "último usado".
  Ganho: elimina recompilação/revalidação de ~340 MPGs/quadro. Se a revalidação hoje custa mesmo que 2-5 µs por MPG,
  são ~1-2 ms/quadro (~3-5 % da thread do jogo a 23 fps); maior se houver recompilação real.
- **Flags mortas por análise para trás** no executor nativo (já há `tools/lazy_vu0_mac.py` para o VU0 gerado): na
  tradução de cada programa, marcar quais pares precisam calcular MAC/status/clip e usar handlers "sem flag" nos outros.
  O MAC/status costuma ser metade do custo de um FMAC vetorial em C. Ganho estimado: 20-35 % do tempo da VU1
  (~5-8 % da thread do jogo).
- **Stalls estáticos**: calcular a contabilidade de ciclos/latência do Q/P e FMAC por programa na tradução, não por
  par em tempo de execução (se o laço hoje faz isso). Ganho: ~5-10 % do tempo da VU1.
- **Estágio 2b/JIT x86-64**: (a) despachar por **bloco** (sequência sem desvio) em vez de por par; (b) VF em xmm dentro
  do bloco, write-back só no fim; (c) flags materializadas no fim do bloco; (d) entrada por `(programa, PC)` com
  atalho. Ordem de grandeza do microVU vs interpretador: 10-30x **[NAO VERIFICADO]**; para nós, de ~30 ns/par para
  2-5 ns/par, isto é, os 23 % da VU1 cairiam para ~3-5 % da thread do jogo.

---

## 3. GS por hardware (GSRendererHW / GSTextureCache)

### 3.1 O que o PCSX2 faz

- **Lote só quebra quando o estado muda de verdade** (lido: `pcsx2/GS/GSState.cpp`): handlers de registrador ligam bits
  em `m_dirty_gs_regs` comparando valor anterior e novo (ex.: `ApplyTEX0` compara `TEX0` com máscara);
  `CheckFlushes()` antes de cada kick chama `TestDrawChanged()` só se há bits sujos e vértices pendentes; se mudou,
  tenta `CanBufferNewDraw()` antes de dar flush. Escrever o mesmo valor (muito comum em listas de VU1) **não quebra o
  lote**. `CheckWriteOverlap()` só dá flush numa transferência que cobre a textura ativa; `TEXFLUSH` só arma um flag.
- **Junção/conversão de draws** (lido: `GSRendererHW.cpp`): `MergeSprite` (sprites em mosaico viram um),
  `ConvertSpriteTextureShuffle` (shuffle de textura vira um quad), `IsPossibleChannelShuffle`/`TestChannelShuffle`
  (shuffle de canal de 8 bits; "Channel shuffles repeat lots of draws. Get out early if we can."),
  `IsSplitTextureShuffle`, `IsStartingSplitClear`/`ContinueSplitClear`/`FinishSplitClear` (série de clears vira um
  sprite), `TryTargetClear` (clear vira clear de textura, sem draw), `CanUseSwSpriteRender`/`SwSpriteRender`
  (sprites pequenos simples desenhados na CPU em vez de draw).
- **Paleta a partir de RT**: `m_mem.m_clut.GetGPUTexture()` (CLUT carregada da GPU, sem readback) e
  `LookupPaletteSource` no cache de texturas; fontes de 8 bits lidas de um alvo de 32 bits são convertidas por shader
  na GPU **[NAO VERIFICADO: nomes `ConvertToIndexed`/`paltex` em `GSTextureCache.cpp`]**.
- **DATE**: `EmulateDATESelectMethod` escolhe entre DATE com framebuffer fetch, por primitive ID
  (`PrimIDTracking`) e uma passada simples quando não há sobreposição (`m_prim_overlap`); `DetermineBarriers` só põe
  barreira com sobreposição.
- **Hacks por jogo (CRC)**: `m_oi`/`OI_*` (`GSHwHack.cpp`) e `GSC_*` pulam ou trocam draws específicos
  **[NAO VERIFICADO: não há entrada para este jogo que eu conheça]**.

### 3.2 O que fazemos

RT reais por (FBP,FBW,PSM), CLUT, render-to-texture; ~733 draws, ~400 quebras de lote e ~207 dispatches por vsync após
otimizar; GS em thread própria. Padrão RT → textura de paleta de 8 bits e ~9 000 sprites auxiliares de 45 px por quadro.

### 3.3 Ideias aplicáveis

- **Quebra de lote por "estado efetivo mudou"** (`m_dirty_gs_regs` + `TestDrawChanged`): comparar o estado de draw
  (TEX0 mascarado, ALPHA, TEST, FRAME, ZBUF, CLAMP, TEX1) com o do lote aberto, ignorando escritas repetidas e campos
  que não afetam o draw (ex.: TEX0 com TME=0, CLUT sem CLD de carga). Ganho na thread GS: ~400 → ~150-250 quebras.
- **Sprites de 45 px como lote único**: os ~9 000 sprites por quadro com o mesmo estado entram num só draw instanciado
  (sprite = 1 instância, expandido no VS), como o caminho de sprite do PCSX2 (expansão de sprite em VS/GS)
  **[NAO VERIFICADO: detalhe do VS expand]**. Se eles alternam alvo/fonte (RT→textura), agrupar por par (alvo,fonte)
  quando não há dependência (o `CheckWriteOverlap` só obriga flush se a fonte é o alvo pendente).
- **Paleta de 8 bits vinda de RT sem readback**: CLUT e indexação lidas da textura do RT na GPU (shader com amostragem
  do índice + lookup na CLUT em textura 256×1); o importante é **nunca** voltar à memória local da CPU no meio do quadro.
- **Clear e cópia como operações de textura** (`TryTargetClear`, `Move` no TC): sprites que só limpam viram
  `ClearRenderTargetView`; transferências local→local viram `CopySubresourceRegion`.
- **SW sprite para casos minúsculos**: só se o draw for menor que o custo fixo de um dispatch (provavelmente não nos
  ~45 px, que valem mais em lote).

---

## 4. Distribuição de threads e limite do EE

### 4.1 PCSX2

- **EE + IOP + VIF/DMA + VU0** na thread da CPU (dynarec); **VU1** na MTVU (opcional, "MTVU speedhack"); **GS** na
  MTGS (anel `SendDataPacket`/`SendSimplePacket`, limite de quadros na fila) **[NAO VERIFICADO: nomes atuais em
  `MTGS.cpp`]**; apresentação no próprio GS thread / host. O EE nunca espera a VU no caminho normal (1.1).
- **Limite prático**: a comunidade trata a thread EE como o gargalo de CPU (single-thread), mitigado por
  `EECycleRate` (underclock), `EECycleSkip` (pular ciclos na VU, visto em `ExecuteVU`), "VU cycle stealing" e
  `INSTANT_VU1` **[NAO VERIFICADO: valores/efeitos]**. Esses hacks mudam o tempo emulado, não o custo do host; para nós
  (código recompilado) o equivalente é reduzir o custo do runtime do EE.
- **Recompilador do EE** **[NAO VERIFICADO nesta sessão]**: `x86/ix86-32/iR5900*.cpp` com encadeamento de blocos
  (block linking), contagem de ciclos por bloco, const propagation e acesso à memória por **vtlb**: um load faz
  `tabela[endereço >> 12]` e, se o ponteiro é direto (RDRAM/scratchpad), acessa sem chamada; só páginas de hardware
  vão a um handler (bit alto da entrada). Endereços constantes conhecidos em tempo de compilação viram acesso direto.

### 4.2 O que fazemos / diferença

Código recompilado estático (sem custo de despacho de bloco), mas cada acesso passa por `isSpecialAddress` + tradução
no runtime (runtime do EE 11,5 % da thread). Diferença principal: o vtlb do PCSX2 resolve o caso comum (RDRAM) com uma
tabela e um teste de bit inline; nós chamamos uma função com vários testes por acesso.

### 4.3 Ideias

- **Fast path de memória inline**: no código gerado, `if ((addr & 0xE0000000) == 0 && addr < 32 MB)` (ou tabela de
  páginas de 4 KB com ponteiro direto) → `rdram[addr & mask]` inline; só o resto chama o runtime. Com endereço base
  constante (`lui`+offset, muito comum), decidir em tempo de recompilação. Ganho: boa parte dos 11,5 % (estimo 5-8 %).
- **Contagem de ciclos por bloco** em vez de por instrução/chamada (se o gerador conta por instrução).
- **Checagem de evento/interrupção só em fronteira de bloco/retorno de função** (equivalente ao `eventTest` do
  dynarec), não em cada acesso.

---

## 5. Lista priorizada de ideias

Ganho em % da thread do jogo (perfil hw2: VU1 23 %, guest+VU0 17 %, runtime EE 11,5 %, ocioso 10 %). Estimativas, não
medições.

| # | Ideia | Ganho estimado | Esforço | Risco | Arquivos nossos afetados |
|---|---|---|---|---|---|
| 1 | Diagnosticar a espera da etapa 4: ETW/WPR na thread do jogo; `timeBeginPeriod(1)` como teste; cronometrar VBlankStart inteiro e contar `yield`/esperas por vsync em todos os laços | destrava o modo `vu` (que pode valer até os 23 % da VU1 em paralelo) | 1-2 h | baixo | `include/ps2_vu_thread.h`, `Kernel/EeScheduler.cpp`, `ps2_runtime.cpp` |
| 2 | MTVU como no PCSX2: **remover o `waitIdle` do VBlankStart** (limite de avanço pela fila do GS, 2 quadros), acordar por escrita com evento sem prazo, PATH1 entregue em ordem pelo EE ou por fila VU→GS com tickets; frontend do GS fora da thread VU1 | 15-20 % (VU1 sai da thread do jogo; sobra o custo do anel) | 4-8 h | médio (ordem PATH1/2/3, MSKPATH3; oráculo de hash já existe) | `ps2_vu_thread.h`, `ps2_memory.cpp`, `gs/gs_frontend.cpp`, `ps2_runtime.cpp`, `Kernel/EeScheduler.cpp` |
| 3 | MPG idêntico não invalida: comparar bytes (ou hash das faixas usadas) antes de `markVU1CodeModified`; programas por conteúdo com atalho "último usado" (`mVUclear`/`mVUsearchProg`) | 3-5 % (mais se hoje houver recompilação real) | 1-2 h | baixo | `ps2_vif1_interpreter.cpp`, executor nativo da VU1 (`vu1-native.md`) |
| 4 | Flags MAC/status/clip mortas por análise para trás no executor nativo da VU1 (`mVUsetFlags`) | 5-8 % | 3-5 h | médio (flags lidas por FMAND/FSAND/branches e pelo programa seguinte: olhar as 4 primeiras do sucessor) | executor nativo VU1, tradutor de programa |
| 5 | Fast path de memória inline para RDRAM/scratchpad no código gerado (estilo vtlb) e decisão em tempo de recompilação para base constante | 5-8 % | 4-6 h + rebuild completo | médio (MMIO/aliases de RDRAM, uncached 0x20000000/0x30000000) | gerador do PS2Recomp (emissão de load/store), `ps2_memory.cpp` |
| 6 | Quebra de lote no GS por estado efetivo (`m_dirty_gs_regs` + `TestDrawChanged`), ignorando escritas repetidas | thread GS: 10-20 %; thread do jogo: 0-2 % (GS já é outra thread) | 2-3 h | baixo | `gs/` backend hw (`gs-hw-backend.md`) |
| 7 | Sprites auxiliares de 45 px em draw instanciado por (alvo, fonte, estado) | thread GS: 10-30 % | 3-5 h | médio (dependência RT→textura entre sprites) | backend hw, shaders |
| 8 | Stalls/ciclos da VU1 calculados por programa na tradução (estáticos), não por par | 1-2 % | 2-3 h | baixo | executor nativo VU1 |
| 9 | JIT/estágio 2b por bloco com VF em xmm e flags no fim do bloco | 15-20 % (VU1 de 23 % para ~3-5 %) | 20-40 h | alto | novo módulo JIT x86-64 |
| 10 | Clears e cópias local→local como operações de textura (`TryTargetClear`, `Move`) | thread GS: 2-5 % | 2 h | baixo | backend hw |
| 11 | Efeitos da VU1 para o EE (SIGNAL/FINISH/LABEL/T-bit) por bits atômicos, como `Get_MTVUChanges` (necessário para tirar outras esperas do EE no modo `vu`) | habilita 2 | 1-2 h | médio | `ps2_memory.cpp`, `gs/gs_frontend.cpp` |

Ordem sugerida para o próximo ciclo: 1 → 3 → 2 (+11) → 4 → 5; 6/7/10 na thread GS só quando ela virar gargalo.

## 6. Fontes (PCSX2, GPL-3.0, só leitura em 05/11/2026)

- `pcsx2/MTVU.cpp` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/MTVU.cpp (lido)
- `pcsx2/Gif_Unit.cpp` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/Gif_Unit.cpp (lido em parte)
- `pcsx2/x86/microVU.cpp` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/x86/microVU.cpp (lido)
- `pcsx2/x86/microVU_Flags.inl` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/x86/microVU_Flags.inl (lido)
- `pcsx2/GS/GSState.cpp` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/GSState.cpp (lido em parte, 0-100 k)
- `pcsx2/GS/Renderers/HW/GSRendererHW.cpp` — https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/HW/GSRendererHW.cpp (lido em parte, 0-100 k e 200-300 k)
- Não lidos nesta sessão (itens marcados [NAO VERIFICADO]): `MTGS.cpp`, `Gif_Unit.h`, `Vif_Unpack.cpp`,
  `microVU_IR.h`, `microVU_Analyze.inl`, `GSTextureCache.cpp`, `GSDevice11.cpp`, `tfx.fx`, `GSHwHack.cpp`,
  `x86/ix86-32/iR5900*.cpp`, `vtlb.cpp`.
- Nosso runtime lido (só leitura): `work/ps2recomp_game/ps2xRuntime/include/ps2_vu_thread.h` (consumidor:
  `cv.wait_for(500 µs)` após 4 000 `yield`; EE: `yield` em `waitIdle` e no anel cheio),
  `src/lib/Kernel/EeScheduler.cpp` (esperas com prazo de host desligadas em guest-clock-only).
