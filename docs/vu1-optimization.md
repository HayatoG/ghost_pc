# Otimização do interpretador VU1 (04/11/2026)

Objetivo: reduzir o custo do `VU1Interpreter` (≈32 % da thread do jogo na gameplay 3D, ver
`gameplay-profile-2026-11-04.md`) **sem mudar nenhum bit da saída**. Escopo: `ps2xRuntime/src/lib/vu/*`,
`include/runtime/ps2_vu1.h` e o harness novo em `ps2xRuntime/tests/vu1_replay/`.

**Resultado:** a saída é bit a bit idêntica (estado, memória do VU e cada pacote GIF) em 15 000 chamadas
reais de gameplay e ~50 000 sintéticas. O mesmo trabalho de VU ficou **~3,0 a 3,3x mais rápido** no
replay de gameplay (mínimo de 10 rodadas intercaladas; a máquina estava carregada por 5 agentes, então o
erro é de ±10 %). A chamada vazia ficou 8,8x mais rápida. A meta de 4x **não** foi atingida: o perfil agora
é plano (ver "Próximos passos").

## Como a equivalência é provada (replay offline)

Rodar o jogo duas vezes não prova nada (threads, relógio de parede) e é lento. Então:

1. **Gravação**: `PS2X_VU1_RECORD=<arq>` [`PS2X_VU1_RECORD_VSYNC=N`, `PS2X_VU1_RECORD_COUNT=M`, padrão 4000]
   grava a entrada de cada chamada de micro-programa: `VU1State`, a memória de código quando ela muda, a
   memória de dados e os argumentos. Grava a partir do vsync N. O código fica em `ps2_vu1_core.cpp`
   (`VuCallRecorder`), desligado por padrão (custo: um teste de ponteiro por MSCAL).
   `work/vu1_bench/record.ps1` roda o jogo com isso.
2. **Replay**: `tests/vu1_replay/build.bat <saida>` compila `vu1_replay.cpp` duas vezes, com as **mesmas
   flags do runtime** (`/O2 /fp:fast /arch:AVX2`…):
   - contra uma cópia congelada do interpretador original (`ref/`);
   - contra o código atual.

   `fake/` substitui `PS2Memory`/`GS` pelo mínimo que o VU usa: os pacotes do XGKICK são coletados e o hash
   é feito fora do tempo medido. Por chamada, o replay grava o hash do estado final, da memória de dados e
   dos pacotes GIF. `cmp` dos dois arquivos de digest é a prova. O replay também mede o tempo só dentro do
   interpretador, numa thread só. Como 4º argumento aceita um arquivo para o amostrador
   (`ps2_sampling_profiler`, resolvido com `tools/resolve_profile.py` e o `.map`). O 5º argumento imprime o
   estado de uma chamada, para achar a diferença.
3. `gen_synthetic.py`: programas aleatórios gerados a partir das tabelas de opcodes válidos, com
   denormais, inf/NaN, valores perto de FLT_MAX/FLT_MIN, DIV/SQRT/RSQRT, EFU, flags e branches curtos.
   Cobre o que a captura do jogo não exercita. Foi ele que pegou o único erro do processo (ver ESUM abaixo).
4. Regressão de imagem: `tools/regress.ps1`. O menu deu idêntico. No scene3d, q07300 e q07500 deram
   idênticos, mas de q07564 em diante há ~1 900 px diferentes, **iguais aos do `work/raster_exe` de outro
   agente** (mesmo md5). Uma captura dos vsyncs 7563–7568 com esse mesmo exe, repassada pelos dois
   interpretadores, é idêntica, então a diferença vem do GS e não do VU1.

Dados usados (fora do git, em `work/vu1_bench/`):
- `rec_gameplay.bin`: 6 000 chamadas, vsync 7560–7562;
- `rec_7563.bin`: 9 000 chamadas, vsync 7564–7568;
- `synth*.bin` e `big*.bin`: 50 000 chamadas sintéticas no total.

No jogo (amostrador da thread do jogo, scene3d, `PS2X_PROFILE_DELAY=245`, 40 s, máquina carregada,
exe de 22:56 que já traz mudanças de outros agentes no GS): o VU1 caiu para **19,7 % das amostras do módulo**
(antes 33,7 %). A razão VU1/rasterizador foi de 0,61 para 0,35. Isso não isola o ganho do VU1, porque o
rasterizador também mudou. A medida final, sem carga, fica com o líder. Saída resolvida:
`logs/vu1_prof_game_new_full.txt`.

Uma surpresa de medida: **~3 000 MSCALs por quadro**, com média de ~245 ciclos de VU por chamada e 88 % dos
ciclos executando par de instruções.

## O que mudou

| # | Mudança | Por quê |
|---|---|---|
| 1 | `commitReadyPipelines`: máscara de bits de slots válidos por pipeline e `m_nextCommitCycle` (menor `readyCycle` pendente). Sai na hora antes disso, e o teste é feito antes da chamada | era 34 % do VU1: varria 51 entradas duas vezes por ciclo. A ordem de commit (pipeline a pipeline, slots em ordem crescente) e a alocação no menor slot livre continuam iguais |
| 2 | `m_xgkick = {}` (zerava o buffer de 64 KB a cada MSCAL e a cada XGKICK) passou a zerar só os campos escalares | o buffer só é lido até os bytes já copiados. Com 3 000 MSCALs por quadro, isso dava ~200 MB de memset por quadro |
| 3 | Cache de decodificação **preguiçoso**: um "epoch" invalida tudo de uma vez, e cada par é decodificado no 1º uso. A validação de código/geração é feita uma vez por `run()`, não por par. O par é acessado por referência em vez de cópia | `rebuildDecodedCodeCache` (2 %) redecodificava 2 048 pares a cada upload de microcódigo |
| 4 | `normalizeOperand` inline no header, com versão SSE2 de 4 lanes em `execUpper` | eram 14 chamadas fora de linha por instrução upper |
| 5 | Flags do FMAC: `fmacResultFlags` decodifica o opcode uma vez e junta `normalizeFmacResult` + `calculateFmacProductSticky`. Há um **caminho rápido provado** (abaixo) | o caminho exato (long double por lane, switch por lane) era ~15 % |
| 6 | Multiplica-soma em `execUpper` com `std::fma` explícito | o otimizador já contraía essas expressões para FMA. O replay dos dados sintéticos aleatórios confirmou que é bit a bit igual. Tornar explícito garante o arredondamento único de que o caminho rápido depende |
| 7 | Stall check e marcação de escrita: as leituras upper/lower são juntadas na decodificação. As lanes de `m_vfReady` são tratadas com AVX2. Os bits de VI são percorridos com `countr_zero` | laços sobre 16 VI e 4 lanes por par |
| 8 | Stalls sem commit pendente e sem PATH1 ativo pulam direto para o próximo ciclo útil (`advanceTo`, `flushPipelines`) | o resultado é idêntico: nesses ciclos só muda o contador |
| 9 | Menores: NOP upper sai cedo; `updateFmacFlags` sem laço de ifs; entradas de pipeline não são zeradas duas vezes; `microAddressMask` inline em `execLower`; cópia de 16 B no XGKICK quando não dá a volta na memória | — |

### O caminho rápido das flags (por que é exato)
Todo resultado FMAC `r` sai de **um único arredondamento para zero** do valor real `x` dos mesmos
operandos normalizados: ADD/SUB/MUL são uma operação, e as formas multiplica-soma usam `std::fma`.

- **Caso normal**: se `r` é normal e `|r| < FLT_MAX`, então `FLT_MIN ≤ |r| ≤ |x| < succ(r) ≤ FLT_MAX` e `x`
  tem o sinal de `r`. Logo o resultado em double não é zero, underflow nem overflow, e as flags são só o
  sinal.
- **Zero exato**: um `r` zero é zero exato quando a operação é soma/subtração de dois floats normais (o
  resultado é múltiplo de 2^-149 e o runtime não liga FTZ) ou um produto com operando zero.
- **Resto**: qualquer outro caso (zero de FMA, denormal, FLT_MAX) vai pelo caminho exato antigo.

### Erro encontrado e corrigido (instrutivo)
Deixar `normalizeOperand` inline mudou o **código gerado** de `ESUM` em `ps2_vu1_lower.cpp`: com `/fp:fast`,
o `0.0f + x` foi eliminado, e um resultado `-0` passou a sair como `-0` em vez de `+0` no registrador P.
O caso apareceu em 1 de 8 000 chamadas sintéticas. A correção: `ps2_vu1_lower.cpp` usa uma cópia
`__declspec(noinline)` (`normalizeOperandOpaque`), que preserva a geração de código original. Lição: com
`/fp:fast`, mudar *onde* uma função é inline pode mudar resultados de ponto flutuante, e só uma comparação
bit a bit pega isso.

### Tentado e descartado
- Flags com AVX em double: igual ao escalar dentro do ruído.
- Filas circulares (FIFO) para flags/VF/ACC/stores no lugar da varredura por máscara: medido 3–8 % **mais
  lento**. Ficou a máscara.

## Header (`ps2_vu1.h`) e build
Todo arquivo gerado do runner inclui `ps2_vu1.h` via `ps2_runtime.h`, então mudar o header recompila tudo.
Ele foi mudado **uma vez**. Além dos campos novos, ganhou:
- `friend struct VU1InterpreterAccess` (helpers privados em `src/lib/vu/`);
- 16 KB de armazenamento opaco (`m_extension`).

Assim, as próximas mudanças no VU não precisam tocar no header. Atenção: misturar o ninja do WinGet (1.13)
com o do VS (1.12) faz um descartar o `.ninja_log` do outro. Use sempre o do `vcvars`
(`work/vu1_bench/ninja_perf.bat`).

## Riscos
- O cache preguiçoso confia em `markVU1CodeModified()` a cada escrita na memória de código, como o
  original. O único caminho que escreve sem marcar é DMA de scratchpad para a memória de código do VU, nunca
  visto. Nesse caso o original usaria código velho e o novo, código novo, só nos pares não decodificados
  desde a última invalidação.
- O caminho rápido das flags depende do arredondamento para zero no MXCSR durante `run()` (já era assim)
  e de FTZ desligado (o runtime não liga). Se alguém ligar FTZ, o argumento do "zero exato" cai: nesse caso,
  remover o ramo `zeroIsExact` de soma/subtração.
- Uma fila de pipeline "cheia" continua reportando o mesmo erro nas mesmas condições.

## Próximos passos (para chegar a 4x ou mais)
O perfil do replay está plano: `run` 17 %, commit 17 %, flags 11–14 %, `execUpper` 9 %, stall check 9 %,
`execLower` 6 %. São ~90 ns por par de instruções na máquina carregada, e o custo agora é de
despacho/branches. O próximo salto é estrutural: "closure compilation" por par na decodificação
(handler por opcode com campos já extraídos, latências e máscaras prontas) e a escrita direta na fila
atrasada, sem o vai-e-vem de cópia do registrador antigo/novo em `run()`.
