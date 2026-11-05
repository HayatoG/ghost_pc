# VU1 nativo: executor traduzido de micro-programas (05/11/2026)

Continuação de `vu1-optimization.md`. **Ligado por padrão** para VU1 e VU0; `PS2X_VU1_NATIVE=0` volta para o
interpretador. Os fontes ficam em `work/ps2recomp_game` (fora do git): o líder precisa levar para `patches/`. Código:
`ps2xRuntime/src/lib/vu/ps2_vu1_native.cpp` (novo), `ps2_vu1_access.h` (novo: a struct `VU1InterpreterAccess`
saiu de `ps2_vu1_core.cpp` para ser compartilhada), gancho de 4 linhas no início de `VU1Interpreter::run` e em
`resetScheduler`. **O `ps2_vu1.h` não mudou** (usa o `friend` e os 16 KB opacos `m_extension`).

## Decisão e porquê

O usuário abandonou o critério "byte-idêntico" para o caminho rápido e pediu o desenho do microVU (tradução
dos micro-programas, sem modelo de pipeline ciclo a ciclo). Mesmo assim, o estágio 1 saiu **bit a bit idêntico,
inclusive no contador de ciclos**, sem custar velocidade. O motivo:

- O interpretador **para (stall) todo leitor de um registrador VF/VI/ACC pendente** até o valor ficar pronto. Então
  a fila de escrita atrasada só muda o resultado nos poucos casos que o próprio interpretador trata à parte:
  1. a instrução lower lê o destino da upper do mesmo par **antes** da escrita (shadow);
  2. um branch logo depois de uma op inteira (IADD, IADDIU, LQI…) lê o VI **antigo** (backup do branch);
  3. o VI é truncado para 16 bits no commit.
  Esses três ficam explícitos no executor; o resto escreve o resultado **na hora**, sem fila.
- O tempo é determinístico (sem cache, em ordem, PATH1 não tem contrapressão). Então dá para pular direto ao
  ciclo de prontidão de cada par, em vez de andar ciclo a ciclo.
- Quem lê **sem stall** é só: MAC/status/clip (FMxx, FSxx, FCxx), Q (ops upper `*q`) e P (MFP). Esses continuam
  nas filas (flags num FIFO próprio; FDIV/EFU nas estruturas do interpretador) e são "commitados" de forma
  preguiçosa, na ordem exata em que o interpretador faria (um ciclo de prontidão por vez; empate: flags, FDIV,
  EFU).
- PATH1 (XGKICK) lê a memória aos poucos (1 qword a cada 2 ciclos). O executor avança o PATH1 **sob demanda**,
  ciclo a ciclo com o próprio `progressXgkick`, imediatamente antes de qualquer store (que poderia mudar o que
  ele lê) e quando precisa saber quando termina (outro XGKICK ou fim do programa). Stores são gravados na
  emissão: no interpretador eles ficam visíveis no limite do ciclo seguinte, antes do próximo par e do PATH1,
  o que dá o mesmo resultado.

Alternativas descartadas por ora: (a) "closure compilation" do interpretador com as filas mantidas (ganho
limitado: o custo era justamente das filas); (b) JIT x86-64 direto (estágio 2): maior risco e mais tempo; só
compensa depois de ver o perfil do estágio 1 no jogo; (c) simular o PATH1 com uma cópia instantânea no
XGKICK, como os emuladores fazem com frequência: muda dados se o programa escreve no buffer durante a
transferência. A recuperação sob demanda é exata e barata.

## Como funciona

- **Tradução por par**, preguiçosa, com cache por instância (2048 pares, ~300 KB no heap, ponteiro em
  `m_extension`). A validade segue a geração do código (`markVU1CodeModified`). Quando a geração muda, cada
  par é revalidado comparando as 2 palavras: um upload parcial não descarta as traduções que não mudaram.
- Cada `NPair` guarda o `DecodedInstructionPair` do interpretador (leituras com stall e latências, a mesma
  fonte de verdade) mais o tipo de handler e os operandos já extraídos.
- **Upper**: ADD/SUB/MUL/MADD/MSUB (bc, q, i, vetor, para VF e ACC), MAX/MINI (comparação de bits crus),
  ITOF/FTOI, ABS, OPMULA/OPMSUB e CLIP rodam em SSE/FMA nas 4 lanes. As flags têm um **caminho rápido
  vetorial** com o mesmo argumento do `fmacResultFlags`: resultado normal → só o sinal; zero exato → Z.
  Novidade: o zero de MADD/MSUB é exato quando `produto == ∓acc` em double (o produto float×float é exato em
  double). Antes 8,7 % das ops FMAC caíam no caminho exato; agora quase só denormal/FLT_MAX caem. O caminho
  exato chama o código original do interpretador (`fmacResultFlags`), fora de linha.
- **Lower**: loads/stores, IADD*/ISUB*/IAND/IOR, todos os branches e jumps, MOVE/MR32/MTIR/MFIR, ILW,
  FC*/FM*/FCGET/FSSET/FCSET, XTOP/XITOP e XGKICK são nativos. DIV/SQRT/RSQRT, EFU, FSEQ/FSAND/FSOR, ILWR,
  RNEXT/RGET/RINIT/RXOR e MFP chamam o `execLower` do interpretador, que escreve na hora e enfileira Q/P nas
  filas dele.
- **Stall**: checagem rápida pelo máximo por registrador (todas as latências de escrita VF são 4). Só se
  houver pendência, ou para FDIV/EFU/PATH1, usa o cálculo exato por lane do interpretador.
- **Fim do programa**: o tempo final é `max(ciclo, última escrita pendente, fim do PATH1, último commit)`,
  como no `flushPipelines`.

## Validação

`tests/vu1_replay` (`build.bat` agora inclui `ps2_vu1_native.cpp`; rodar com `PS2X_VU1_NATIVE=1`), contra o
interpretador original congelado (`ref/`). Compara, por chamada, o hash do `VU1State` inteiro (com `cycles`), da
memória de dados e de **todos os pacotes GIF**:

| Conjunto | Chamadas | Resultado |
|---|---|---|
| `rec_gameplay` (vsync 7560–7562, inclui VU0) | 6 000 | idêntico |
| `rec_7563` (vsync 7564–7568, inclui VU0) | 9 000 | idêntico |
| `synth`, `synth2`–`4`, `big21`, `big22` (sintéticos) | ~50 000 | idêntico |
| `synth_n1`, `synth_n2` (sementes novas 777 e 4242) | 40 000 | idêntico |

Script: `work/vu1_bench/nat/validate.sh`. Com `PS2X_VU1_NATIVE=0`, o interpretador continua idêntico à
referência.

Regressão de imagem (`tools/regress.ps1`, rasterizador de CPU, tolerância zero) com o exe integrado (build de
01:05, cópia em `work/vu1_bench/exe_nat`, `PS2X_VU1_NATIVE=1`): **menu idêntico (md5)** e **scene3d com os 6
quadros idênticos (md5)**, de q07300 a q07700. Atenção: o menu não executa nenhum micro-programa (o log
`[VU] micro-program executor` nem aparece), então a prova vem do scene3d.

**Padrão desde 01:18**: o executor nativo fica ligado sem variável. O scene3d foi repetido com o exe padrão
(01:18, `work/vu1_bench/exe_def`, sem variável; o log confirma `native`): os 6 quadros saíram idênticos. `PS2X_VU1_NATIVE=0` volta para o
interpretador. No stderr sai uma linha `[VU] micro-program executor: native|interpreter` na primeira chamada.

Divergências conhecidas (nenhuma aparece nos dados): só em chamadas que o interpretador **não termina**
(orçamento de 65 536 ciclos esgotado, ou opcode reservado). Nesses casos, os resultados que ainda estariam na
fila já estão nos registradores, e um erro de tag no PATH1 é detectado quando o PATH1 é alcançado, não no ciclo
exato. O trace de pares do `PS2X_GS_DUMP_VSYNC` (`[vu1-pair]`) não é emitido no modo nativo.

## Resultados

Uma medida no harness (`rec_gameplay`, 30 passadas, máquina carregada por outros agentes, sob o amostrador):
**~28 ns por ciclo de VU** no executor nativo. Para comparar, a mesma medida com o interpretador atual deu
~113 ns por ciclo na mesma sessão. No jogo (uma medida: amostrador da thread do jogo, `scene3d.txt`,
`PS2X_PROFILE_DELAY=135`, 40 s, máquina com outros agentes rodando; `logs/vu1_native_prof_full.txt`): todo o
código de VU somou **5,9 % das amostras do módulo** (o executor nativo mais os restos do interpretador). Em
`vu1-optimization.md` eram 19,7 % (33,7 % no original), mas a janela não é a mesma: com o jogo mais rápido, o
atraso de 245 s passou do fim do script, e a janela de 135 s inclui trechos de carga (IOP 22 %). A medida
final, com a máquina parada, fica com o líder.

Perfil do executor no harness: laço `run` ~23 %, flags/armazenamento FMAC ~14 %, commit preguiçoso ~13 %,
lower ~11 %, marcação de escritas ~8 %. Ficou plano: o próximo salto é o estágio 2.

## Estágio 2a: entradas de MAC/status que ninguém lê

A entrada de flags do par k é descartada (e os bits sticky dela, `current | extraSticky`, vão para a próxima
entrada) quando, em linha reta, o par k+1 enfileira a própria entrada e nenhum par de k+1 a k+4 lê MAC/status.
Esses são os únicos pares que podem ser emitidos dentro de `[ready(k), ready(k+1))`: um stall só os atrasa.
Também é preciso que nenhum branch em k..k+2 mude quais pares são esses, que não haja FSSET em k ou k+1 (ele
derruba a escrita de status do mesmo ciclo) e que k não tenha bit D/T. Na execução, k não pode ser delay slot
nem o último par (`branchPending`, `ebit`, `haltAfterDelaySlot`). A tradução guarda as palavras dos 4 pares
seguintes, de que a decisão depende, e as revalida quando o código muda. Isso cortou o commit de flags pela
metade no perfil do harness, com resultado idêntico em todos os conjuntos. Equivale ao `flagSet` do microVU,
mas sem eliminar o cálculo das flags (os bits sticky ainda são necessários).

## Estágio 2b (não feito): o que vem a seguir

1. **Flags sem cálculo**: um acumulador de sticky por bloco e flags calculadas só nos pares observados.
2. **Blocos**: agrupar os pares até o branch/E-bit e resolver os stalls estaticamente, com o estado do
   pipeline na entrada como parte da chave (o `microRegInfo` do microVU).
3. **Emissor x86-64** dos blocos (VirtualAlloc executável, xmm fixos por bloco), mantendo os handlers atuais
   como fallback.

## Rodada de 05/11 (03:59–04:15): medida para o estágio 2b, sem mudança de código

Prazo curto (16 min): só houve tempo para medir e decidir o próximo passo. Nenhum arquivo de código foi alterado.

Medida (harness, `rec_gameplay`, 10 passadas, 3 repetições, exe do estágio 2a de 01:10, máquina com outros
agentes): **25,0–25,8 ns por ciclo**; 6 000 chamadas e 1 470 304 ciclos de VU por passada, ou seja **~245 ciclos
por chamada** e **~6,1 µs por chamada**. A gravação cobre 3 vsyncs, então são ~2 000 chamadas por vsync e
**~12 ms por vsync**, o que bate com os ~11,3 ms do perfil do jogo (`gameplay-profile-hw2-2026-11-05.md`).

Leitura: os micro-programas do jogo são curtos (~245 ciclos). Isso muda a prioridade do estágio 2b:
1. **Custo fixo por chamada e revalidação após MPG** (candidato a): a cache é invalidada por *geração global*
   do microcódigo (`getVU1CodeGeneration`). Com ~340 MPGs por quadro, quase toda chamada encontra geração nova
   e revalida cada par executado (comparação de 8 bytes + `memcmp` de 32 bytes do lookahead). Com ~100 pares
   distintos por chamada, a estimativa é ~1 µs de 6,1 µs (não medido ainda). Correção proposta: a memória
   registrar a faixa suja de cada MPG (geração por bloco de 512 bytes, por exemplo) e o executor revalidar só
   os blocos cujo número mudou. Exige mexer em `PS2Memory` (fora do escopo desta rodada).
2. Também por chamada: `fegetround`/`fesetround` (dezenas de ns) e o `syncTo` do começo; pequenos frente a 6 µs.
3. Depois, o laço por par (estágio 2b itens 1–3 acima). Com programas de ~245 ciclos, um JIT só compensa com
   cache de blocos traduzidos chaveada pelo conteúdo (hash do bloco), porque o código muda a cada quadro.

Próximo passo concreto: instrumentar no harness o tempo de `fetch` com geração nova contra geração igual
(rodar `rec_gameplay` com a geração congelada) para medir o item 1 antes de implementá-lo.

## O que estudei do microVU (PCSX2, GPL-3.0+), regra 8

Estudo do desenho de memória, mais uma leitura (somente leitura) de
`https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/x86/microVU_Analyze.inl` (SPDX `GPL-3.0+`, PCSX2 Dev
Team). **Nenhum trecho foi copiado**; o código é próprio e segue o interpretador deste projeto. Ideias aproveitadas e onde elas estão:
- análise estática de stalls por instrução (`analyzeReg*`/`mVUstall`): aqui, a checagem por registrador mais o
  cálculo exato por lane;
- instâncias de flags e eliminação das não observadas (`flagSet`, `mFLAG`/`sFLAG`): aqui, o FIFO de flags (a
  eliminação fica para o estágio 2);
- Q/P com latência pendente (`analyzeQreg`/`analyzePreg`) e XGKICK com ciclos de transferência
  (`mVUanalyzeXGkick`);
- leitura do VI do branch olhando instruções anteriores (`analyzeBranchVI`): aqui, o backup do branch do
  interpretador.

O PS2Recomp também é GPL-3, então não há conflito de licença.

## Riscos

- O caminho rápido das flags depende do arredondamento para zero no MXCSR (o `run` ativa) e de FTZ/DAZ
  desligados, como no interpretador.
- O FIFO de flags supõe latência igual (4) para todas as entradas. Isso vale hoje: se alguém mudar
  `kFmacLatency` por tipo de entrada, a ordem de commit precisa voltar a ser por ciclo.
- A checagem rápida de stall supõe latência VF igual (4) para todas as escritas VF (`regReady` = máximo por
  registrador). Se uma latência VF mudar, `readyCycle` continua correto só se o máximo for mantido: é o que
  `markWrites` faz com `std::max`.
- Misturar interpretador e nativo na mesma instância é tratado (as filas restantes do interpretador são
  commitadas), mas não foi exercitado.
