# Reduzir o custo de rebuild do runner (41,8 mil .cpp gerados)

Data: 2026-11-04. Autor: análise de agente (sem tocar em `work/ps2recomp_game`; tudo em `work/build_speedup/` e num sandbox de `cl.exe`). Log bruto: `logs/build_speedup_20261104.log`. Rascunho/patch pronto: `work/build_speedup/` (seção 7).

## 1. Resumo

- **O runner já usa unity build (lotes de 32) e PCH** (`PS2X_ENABLE_RUNNER_UNITY_BUILD=ON`, `PS2X_ENABLE_RUNNER_PCH=ON`, `PS2X_ENABLE_LTCG=OFF` em `out/build_perf`). As opções (b) e (c) já estão no ar; o problema não é falta delas.
- **Causa dos rebuilds totais:** o PCH (`cmake_pch.hxx`) inclui `ps2_runtime.h`, e o ninja põe o `.pch` como dependência implícita das **1307 unidades unity**. Qualquer edição em qualquer um dos **22 headers / 46 665 linhas** do fechamento do PCH (incluindo `ps2_vu1.h`, `gs/gs_frontend.h`, `gs/ps2_gif_arbiter.h`, `ps2_memory.h`, `ps2_audio.h`...) refaz o PCH e **todas** as unidades. Hoje, as edições de `ps2_vu1.h` (21:50), `gs_frontend.h` e `ps2_gif_arbiter.h` (22:16) estavam nesse conjunto.
- **O código gerado quase não usa o runtime:** só chama cerca de 25 métodos de `PS2Runtime` por ponteiro (`dispatchGuestBranch`, `eeCheckpointDue`, `Load*/Store*`, `SignalException`, `handleSyscall`, ...) e usa `R5900Context`, as macros e `Ps2IsSpecialAddress`. Nada de GS/VU1/áudio/pad/VFS.
- **Recomendação nº 1 (ganho enorme, risco baixo): header-prelude "leve" (`ps2_runner_prelude.h`).** O fechamento do PCH cai de 22 para **11 headers** e deixa de conter `ps2_runtime.h`, `ps2_vu1.h`, `gs/*`, `ps2_memory.h`, `ps2_audio.h`, `ps2_pad.h`, `ps2_vfs.h`. Editar esses headers passa a custar só as unidades do runtime que os incluem + link (**~30–40 s em vez de ~12 min+**). Medido na amostra: **assembly byte-idêntico** (dumpbin /disasm) e 0 erros de compilação em 3334 arquivos do runner + `register_functions.cpp`.
- **Recomendação nº 2 (iteração rápida): árvore `out/build_dev` com o runner em `/Od`** (`-DPS2X_RUNNER_OPT=dev`): backend das unidades pesadas cai 4–50x (soma da amostra de 11 unidades: 203 s -> 44 s; memória de pico 2,4 GB -> 0,5 GB). Só para iterar, nunca para medir FPS.
- Não recomendados: unity maior/menor sem medir em escala, LTCG (já falhou), sccache (não instalado; não ajuda com PCH).

## 2. Medições

Ambiente: Ryzen 7 5700X3D (8C/16T), 31,9 GB RAM (≈21 GB livres durante as medições), MSVC 14.44, `cl.exe` direto com as flags de `out/build_perf/compile_commands.json` (somente leitura), **1 job**, máquina compartilhada com outros agentes, então há ruído. Use `c1xx+c2` (tempo dentro do compilador, `/Bt+`) para comparar; o "wall" do harness inclui ~2 s do `vcvars64`.

### 2.1 A árvore hoje

| Item | Valor | Fonte |
|---|---|---|
| Arquivos em `src/runner` | 41 808 `.cpp`, 254,8 MB, 6,18 M linhas | contagem |
| Unidades de compilação | 1307 unity (lote 32) + 1 PCH + 55 do runtime | `compile_commands.json` |
| Build completo (último `.ninja_log`) | 720 s de parede no `-j16`; soma 13 332 s de CPU **inflada por contenção** (ver 2.5) | `.ninja_log` |
| Link `ps2EntryRunner.exe` | 9,6–13,3 s; `ps2_runtime.lib` 0,1–2,4 s; cada `.cpp` do runtime 7–11 s | `.ninja_log` |
| Distribuição por unidade (log) | mediana 2,3 s; p90 23,8 s; p99 131 s; máx 231 s. **58 unidades (4%) = 48% do CPU**; 151 unidades (>=20 s) = 72% | `work/build_speedup/ninja_log_stats.py` |

### 2.2 Fan-in dos headers

Includes diretos nos 41 808 arquivos do runner (parse das diretivas): `ps2_runtime.h`, `ps2_syscalls.h`, `ps2_stubs.h` = 41 808 (100%); `ps2_log.h` 41 807; `ps2_runtime_macros.h`, `<ps2_recompiled_functions.h>`, `<ps2_recompiled_stubs.h>` e `<stdexcept>` ≈ 41 598 (99,5%). O resto é o fechamento transitivo (de `.ninja_deps` do `cmake_pch.cxx.obj`):

| Header (fechamento do PCH atual: 22 headers, 46 665 linhas) | Linhas | Editado hoje? | No fechamento leve? |
|---|---:|:-:|:-:|
| `ps2_recompiled_functions.h` (gerado: 41 808 declarações) | 41 817 | não (regen) | sim |
| `ps2_runtime_macros.h` | 1208 | 07:21 | sim |
| `ps2_call_list.h` | 693 | — | sim |
| `ps2_runtime.h` | 555 | 22:38 (ontem) | **não** |
| `runtime/ps2_memory.h` | 461 | 06:36 | **não** (só `ps2_memory_map.h`, constantes) |
| `runtime/ps2_vu1.h` | 340 | **21:50** | **não** |
| `runtime/gs/gs_types.h` | 315 | — | **não** |
| `runtime/gs/gs_frontend.h` | 263 | **22:16** | **não** |
| `ps2_recompiled_stubs.h` (gerado) | 216 | 04:47 | sim |
| `ps2_log.h`, `ps2x/iop/*`, `ps2_vfs.h`, `ps2_audio.h`, `ps2_pad.h`, `ps2_rom_device.h`, `gs/ps2_gif_arbiter.h` (**22:16**), `gs/gs_backend.h` | 710 | parte | **não** |
| `ps2_address.h`, `ps2_syscalls.h`, `ps2_stubs.h`, `Stubs/Unimplemented.h` | 123 | — | sim |

Pré-processado de um arquivo típico: **138 533 linhas** (cheio) vs **97 817** (leve); 41 817 são do `ps2_recompiled_functions.h`.

### 2.3 Custo de compilar e parte de parse de headers

| Caso (1 job, `/O2`) | c1xx (frontend) | c2 (backend) |
|---|---:|---:|
| 1 arquivo típico, sem PCH, máquina muito carregada | 5,21 s | 0,10 s |
| Unity de 32 arquivos triviais, sem PCH | 2,65 s | 0,16 s |
| Criar o PCH cheio (hoje) | 2,6–3,0 s | 0,02 s |
| Criar o PCH leve (proposto) | **1,06–1,14 s** | 0,02 s |
| Unity de 32 triviais com PCH (cheio ou leve) | 0,07–0,09 s | 0,17–0,23 s |
| Unidade pesada (32 arquivos, 1,1 MB) com PCH | 1,06–1,16 s | **24,9–27,6 s** |
| Arquivo único pesado (`FUN_002c6958`, 2,9 MB) | 3,6 s | 22,1 s |

Conclusão: **o parse de headers não é o custo por arquivo** (PCH + unity já o amortizam: 0,07 s). O custo do rebuild completo é (1) o backend `c2` das ~150 unidades pesadas e (2) **o fan-out do PCH**: uma edição de header refaz tudo. O prelude ataca (2); `/Od` ataca (1).

### 2.4 Amostra de unidades reais, `/O2` vs `/Od` (cada um com PCH leve de flags casadas; c1xx+c2 em s, pico de RAM do `cl`)

| Unidade (tempo no log) | `/O2` | pico RAM | `/Od` | pico RAM | Razão |
|---|---:|---:|---:|---:|---:|
| 609 (1,5 s) | 0,36 | 34 MB | 0,11 | — | 3x |
| 881 (25,9 s no log!) | 1,26 | 113 MB | 0,19 | — | 7x |
| 1271 (5,4 s) | 0,55 | 107 MB | 0,17 | — | 3x |
| 468 (8,0 s) | 0,86 | 111 MB | 0,19 | — | 5x |
| 22 (19,8 s) | 11,5 | 420 MB | 0,77 | 60 MB | 15x |
| 40 (44,9 s) | 19,1 | 1042 MB | 1,41 | 131 MB | 14x |
| 114 (89,8 s) | 14,8 | 777 MB | 1,47 | 170 MB | 10x |
| 89 (130,9 s) | 18,1 | 647 MB | 1,67 | 165 MB | 11x |
| "unity_h" (32 arqs, 1,1 MB) | 28,8 | 1130 MB | 1,76 | 140 MB | 16x |
| 143 (231 s, a mais lenta) | 91,1 | **2368 MB** | 20,6 | 517 MB | 4,4x |
| 1301 (`register_functions.cpp`, 18 MB) | 16,9 | 954 MB | 15,6 | 943 MB | 1,1x |
| `FUN_002c6958` sozinho | 25,7 | **3332 MB** | 22,7 | 570 MB | 1,1x |
| **Soma da amostra (11 unidades)** | **203** | | **44** | | **4,6x** |

`/O2 /Ob0` (sem inline automático) deu só 1,8x (unidade pesada: c2 27,6 -> 15,4 s) e não é recomendado. Há uma cauda que **não** melhora com `/Od`: funções gigantes (143, 1301, `FUN_002c6958`) custam 14–23 s em qualquer nível; é custo da própria função (o gerador poderia dividi-las; não avaliado).

### 2.5 Observação sobre contenção (importante)

Os tempos do `.ninja_log` estão **muito inflados** em relação ao isolado: unidade 143 = 231 s no log vs 93 s isolada; 89 = 131 s vs 20 s; 114 = 90 s vs 17 s; 881 = 26 s vs 1,3 s. Com 16 jobs numa máquina de 8 núcleos + outros agentes, e picos de **1–2,4 GB por `cl`** nas unidades pesadas (3,3 GB num arquivo gigante), há paginação/concorrência. Estimativa (não medida em escala): a soma isolada do `/O2` é ~4–5 mil s de CPU, não 13 mil. **Limitar o `ninja -j` (8–10) tende a não piorar o tempo de parede e evita swap** quando outros agentes estão compilando [NAO MEDIDO: precisa de A/B com a máquina ociosa].

### 2.6 Lotes do unity (c)

Unidade pesada de 32 arquivos, PCH leve, `/O2` (soma c1xx+c2 em s):

| Lote | Tempo total | Observação |
|---|---:|---|
| 32 arquivos em 1 unidade (hoje) | ~29 | |
| 8 arquivos x 4 unidades | 48,4 | +70% |
| 1 arquivo x 32 TUs | 57,9 (wall 60,8) | +100%; funções `inline`/macros são re-otimizadas em cada TU |

Triviais: 1 unidade = 0,24 s; 32 TUs = 1,7 s dentro do compilador (+~0,09 s de lançamento de processo cada, wall 4,2 s). O unity de 32 é bom para build completo. O custo incremental é o da unidade inteira (mediana 2 s isolada, até ~90 s na pior), mas edições no **runner gerado** são raras (só no ciclo de regeneração); o que dói são edições de **headers do runtime**, e essas o prelude elimina. Lote 64/128: [NAO MEDIDO] (tendência: build completo mais barato, unidade incremental maior; RAM de pico é dominada pela maior função, não pelo tamanho do lote).

## 3. O que o código gerado realmente usa

Varredura dos 41 808 arquivos + `ps2_runtime_macros.h` (`work/build_speedup/check_runner_prelude.py` repete isso):

- Métodos de `PS2Runtime` chamados por `runtime->`: `dispatchGuestBranch` (52 588 chamadas), `eeCheckpointDue` (8422), `Store32/64/128/8`, `Load32/64/128/16/8`, `SignalException`, `handleSyscall`, `handleBreak`, `handleTrap`, `handleTLBR/WI/WR/P`, `clearLLBit`, `executeVU0Microprogram`, `vu0StartMicroProgram`, `kickGifDmaChainFromMMIO` (emissão do gerador) e `PS2Runtime::isSpecialAddress`, `PS2Runtime::GuestBranchKind`, `PS2Runtime::RecompiledFunction`.
- Tipos/helpers: `R5900Context` (struct puro), `getRegU32`, `setReturnU32/S32/U64`, `PS2Exception`, `PS2_RAM_MASK/SIZE` e as constantes do mapa de memória (via `Ps2IsSpecialAddress`), macros de `ps2_runtime_macros.h` (`GPR_*`, `READ*/WRITE*`, `FPU_*`, `PS2_V*`), `ps2_syscalls::*`, `ps2_stubs::*`, `g_ps2RecompiledFunctionTable*`.
- Nunca: membros de `PS2Runtime`, `sizeof`, `new`, herança, `cpu()/gs()/vu1()/memory()`.

## 4. Opções avaliadas

### (a) `ps2_runner_prelude.h` — RECOMENDADA

Desenho (patch pronto, seção 7):
1. `R5900Context` + `PS2Exception` + helpers saem de `ps2_runtime.h` para **`ps2_r5900_context.h`** (movido, sem mudar uma linha); as constantes do mapa de memória saem de `ps2_memory.h` para **`runtime/ps2_memory_map.h`**; `ps2_address.h` passa a incluir só esse.
2. **`ps2_runner_prelude.h`**: com `PS2X_RUNNER_LIGHT` declara uma classe `PS2Runtime` enxuta (só os 29 métodos/tipos da seção 3, assinaturas idênticas) e os `extern g_ps2RecompiledFunctionTable*`; sem a macro, apenas inclui `ps2_runtime.h` (modo completo, comportamento de hoje).
3. `ps2_runtime_macros.h`, `ps2_syscalls.h`, `ps2_recompiled_stubs.h` passam a incluir o prelude no lugar de `ps2_runtime.h`; `ps2_stubs.h` pula `ps2_memory.h` no modo leve.
4. CMake: o PCH ganha `ps2_runner_light_on.h` (define `PS2X_RUNNER_LIGHT`) como **primeiro** header e usa o prelude no lugar de `ps2_runtime.h`. `main.cpp`, `ghost_overrides.cpp`, `ps2_debug_panel.cpp` (e os de Vita/Android) ficam fora do PCH e do unity (`SKIP_PRECOMPILE_HEADERS`, `SKIP_UNITY_BUILD_INCLUSION`) e enxergam a classe completa. Opção `PS2X_ENABLE_RUNNER_LIGHT_HEADERS` (padrão ON; OFF volta ao comportamento atual).
5. **Onde mudar o include emitido pelo gerador** (`ps2xRecomp/src/lib/`): `function_emitter.cpp:47`, `ps2_recompiler.cpp:107` (preâmbulo do arquivo combinado), `ps2_recompiler.cpp:1350` (arquivos de stub), `ps2_recompiler.cpp:1806` (`ps2_recompiled_stubs.h`), `function_table_emitter.cpp:149` (`register_functions.cpp`): `#include "ps2_runtime.h"` -> `#include "ps2_runner_prelude.h"`. O `apply_build_speedup.py` faz isso e também reescreve o include nos 41 808 `.cpp` já gerados (evita regerar).

Por que não um include "mínimo" sem tocar na classe: o gerador emite `runtime->método(...)`; sem uma declaração da classe com esses métodos o código gerado não compila. A alternativa limpa (funções livres `ps2r_Load32(runtime, ...)` + incompleto `class PS2Runtime;`) exige alterar ~25 pontos de emissão do gerador (`instruction_translator`, `special_translator`, `control_flow_emitter`, `cop0_translator`, `regimm_translator`, `vu_translation_helpers`, `gif_dma_kick_analyzer`) e as macros; a classe com **duas definições** é uma violação de ODR "benigna" (sem virtuais; símbolos MSVC dependem só de nome+assinatura; o código gerado nunca toca em membros). Mitigações incluídas: `check_runner_prelude.py` (alvo CMake `check_runner_prelude`) confere assinatura por assinatura contra `ps2_runtime.h` e que todo `runtime->X` do runner está declarado; se algo desviar, o **link falha** (símbolo não resolvido). Refatorar `PS2Runtime` para pimpl/ponteiros com forward declarations (o jeito "certo" a longo prazo) fica como evolução, pois mexe no coração do runtime onde outros agentes trabalham.

Medição na amostra (flags de `build_perf`, `/O2`, PCH casado):
- PCH criado em **1,1 s** (era 2,6–3,0 s); pré-processado 97,8 k vs 138,5 k linhas; fechamento 11 vs 22 headers.
- **Assembly idêntico**: `dumpbin /disasm:nobytes` da unidade pesada (135 404 linhas) e de uma unidade trivial (6243 linhas): as únicas linhas que diferem são nome do `.obj`, tamanho de `.debug$S`/`.drectve`.
- **0 erros** em `/Zs` de 3334 arquivos (1834 não-função: stubs, `_start`, etc., + 1500 aleatórios) e de `register_functions.cpp` com o prelude; a visão completa (`main.cpp`-like, sem a macro) também compila.
- Efeito no ciclo: editar `ps2_vu1.h`/`gs/*`/`ps2_audio.h`/`ps2_pad.h`/`ps2_vfs.h`/`ps2_memory.h`/`ps2_runtime.h` -> só os `.cpp` do runtime que incluem + `ps2_runtime.lib` + link (7–11 s por `.cpp` + 0,1–2,4 s + 10–13 s ≈ **30–40 s**), contra um rebuild completo (~12–14 min sem contenção, horas com a máquina cheia). **Um** rebuild completo é inevitável ao aplicar (o include emitido muda).
- Continuam invalidando tudo: `ps2_runtime_macros.h`, `ps2_runner_prelude.h`, `ps2_call_list.h`/`ps2_syscalls.h`/`ps2_stubs.h` (novos nomes de HLE; poderiam ser isolados com a mesma técnica, não feito), `ps2_recompiled_*.h` (regeneração).

### (b) PCH — já existe

Está ON. Ganho que já dá: parse do PCH 2,6 s uma vez + 0,07 s por unidade, contra 2,65 s por unidade sem PCH (~0,07 s vs 2,65 s no unity de 32). O problema é o escopo do PCH (22 headers voláteis); (a) o resolve. Nada a mudar além da lista de headers.

### (c) Unity — já existe (lote 32)

Ver 2.6. Manter 32. Para teste de lote maior, uma unidade já leva até ~90 s isoladas (RAM ~2,4 GB), então 64 dobraria os piores casos; não vale sem medição em escala [NAO MEDIDO].

### (d) Árvore `out/build_dev` com flags baratas para o runner

Implementado no patch como `PS2X_RUNNER_OPT=dev` (só o alvo `ps2EntryRunner`; o `ps2_runtime` e o GS continuam `/O2`): `/Od /Ob0 /Zc:inline /fp:fast /arch:AVX2 /GS- /Qspectre-`, sem `/GL`, sem `/OPT:REF/ICF`.

| Alternativa | Build completo estimado (esta máquina, ociosa) | Edição de header do runtime (depois do prelude) | Observação |
|---|---|---|---|
| Hoje (`build_perf`, O2, unity 32, PCH cheio) | ~12–14 min (medido: 720 s `-j16`; spike: ~14 min `-j10`) | = build completo | |
| + prelude (a) | igual (−1,5 s no PCH) | **~30–40 s** | **1ª prioridade** |
| + `build_dev` (`/Od`) | **~3–4 min** [ESTIMATIVA: soma isolada da amostra 203 s -> 44 s; piso de ~0,4–2 s por unidade x 1307 + cauda de ~16–21 s das unidades 143/1301] | ~30–40 s | só iteração |
| `/O2 /Ob0` | ~7–8 min [ESTIMATIVA, 1,8x na unidade pesada] | | pouco ganho, não recomendado |
| Sem unity (1 TU/arquivo) | ~2x o CPU da unidade pesada, ~7x nas triviais | | pior |
| Lote 8 | ~1,7x o CPU da unidade pesada | incremental menor | só se editar runner gerado com frequência |
| LTCG ON | falhou no link (Stack overflow, spike) | | nunca |

Riscos do `dev` [NAO MEDIDOS, validar antes de confiar]: (1) **estouro de pilha do host**: quadros em `/Od` são maiores e cada chamada guest é uma chamada C++ aninhada; os 256 MB (`PS2X_STACK_RESERVE_MB`) foram dimensionados para `/O2`: se estourar (`0xC00000FD`), subir para 512 MB só nesta árvore; (2) **velocidade do código guest** bem menor: `realtime_pct` não é comparável; (3) **`/fp:fast` sem otimização não contrai FMA igual ao `/O2`**: o resultado em ponto flutuante do guest (FPU/VU0) pode divergir em bits, então a captura do `regress.ps1` pode não ser byte-idêntica à de `build_perf`. Se divergir, gravar uma referência própria do `build_dev` (`-Record` com o **exe de dev** só para comparar dev vs dev) e manter a referência de `build_perf`/baseline para decisões. **Nunca medir FPS nem gravar referência oficial com `build_dev`.**

### (e) ccache/sccache

Não há `sccache`/`ccache`/`buildcache` instalados (verificado no PATH). O CMake do `ps2xRuntime` **já** tem `PS2X_ENABLE_SCCACHE` (usa `sccache` como `COMPILER_LAUNCHER` se existir; `build_perf` está com OFF). Viável no Windows (`winget install Mozilla.sccache`; **não instalei nada**), mas: (1) o `sccache` não cacheia compilações com PCH (`/Yu`/`/Yc`) — [conforme a documentação do sccache, não testado aqui], e o runner depende do PCH; (2) o problema aqui não é recompilar entradas idênticas, e sim headers que mudam de verdade; (3) só ajudaria a alternar entre árvores com **as mesmas flags**. Conclusão: sem ganho relevante para este caso; não instalar por enquanto.

## 5. Recomendação ordenada (ganho/risco)

| # | Ação | Ganho | Risco | Custo para aplicar |
|---|---|---|---|---|
| 0 | Hoje, sem mudar nada: não editar os 22 headers do fechamento do PCH quando houver alternativa (`ps2_vu1.h`, `gs_*.h`, `ps2_gif_arbiter.h`, `ps2_memory.h`, `ps2_audio.h`, `ps2_pad.h`, `ps2_vfs.h`, `ps2_runtime.h`); preferir `.cpp` ou headers privados de `src/lib`. Rodar o ninja com `-j8..10` com outros agentes compilando (picos de 1–2,4 GB por unidade pesada) | evita rebuilds totais; menos paginação | nenhum | zero |
| 1 | **Prelude leve** (patch) | **elimina o rebuild total por edição de header do runtime** (12+ min -> 30–40 s) | baixo: assembly idêntico na amostra; ODR "benigno" guardado por script + link; `PS2X_ENABLE_RUNNER_LIGHT_HEADERS=OFF` volta ao normal | 1 rebuild completo uma vez (~12 min) + coordenação (toca `ps2_runtime.h`, `ps2_memory.h`, macros e o gerador) |
| 2 | **`build_dev`** (`PS2X_RUNNER_OPT=dev`) | build completo ~3–4 min; RAM de pico 5x menor | médio: pilha, FP bit-a-bit, velocidade; só iteração | 1 build dev (~3–4 min) |
| 3 | Dividir funções gigantes no gerador (143/1301/`FUN_002c6958`) | cauda de 15–90 s por unidade | médio (mexe no gerador) | [NAO MEDIDO] |
| 4 | sccache | desprezível aqui (PCH) | — | instalar |
| — | Lote do unity / LTCG | sem evidência a favor | — | — |

## 6. Como aplicar e validar (com o `BUILD.lock.d`, ninja/cmake parados)

Todos os caminhos relativos a `ghost_pc/`. Ferramentas: VS ninja (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`) e o CMake do VS (`...\CMake\CMake\bin\cmake.exe`), **nunca o ninja do WinGet** (o `CMakeCache.txt` atual ainda aponta para ele em `CMAKE_MAKE_PROGRAM`: ao reconfigurar, passe `-DCMAKE_MAKE_PROGRAM=<ninja do VS>`).

1. **Baseline antes** (para provar identidade): `powershell -File tools\regress.ps1 -Scenario menu` com o exe atual de `build_perf` (deve dar exit 0). Guardar o exe: `copy work\ps2recomp_game\out\build_perf\ps2xRuntime\ps2EntryRunner.exe work\baseline_pre_speedup.exe`.
2. Pegar o lock; conferir que não há `ninja`/`cl` rodando nessa árvore.
3. Ensaio: `python -I work\build_speedup\apply_build_speedup.py work\ps2recomp_game --dry-run` (valida as âncoras sem escrever).
4. Aplicar: `python -I work\build_speedup\apply_build_speedup.py work\ps2recomp_game` (backup dos arquivos tocados em `work\build_speedup\backup_<data>\`; idempotente; reverter com `--revert <pasta>`).
5. Conferir o prelude: `python -I work\ps2recomp_game\ps2xRuntime\tools\check_runner_prelude.py work\ps2recomp_game\ps2xRuntime\include work\ps2recomp_game\ps2xRuntime\src\runner` (esperado: `0 divergencias`, exit 0).
6. Reconfigurar e compilar `build_perf` (rebuild completo único, ~12 min): `cmake -S work\ps2recomp_game -B work\ps2recomp_game\out\build_perf -DCMAKE_MAKE_PROGRAM=<ninja VS>` e `ninja -C ...\build_perf`. Conferir a mensagem do CMake e o `ps2EntryRunner.exe`.
7. **Identidade de comportamento**: `powershell -File tools\regress.ps1 -Scenario menu -MaxErr 0 -MaxDiffPx 0` (esperado: exit 0, byte-idêntico) e, opcional, `-Scenario scene3d`. Verificação extra e barata do código gerado: `dumpbin /disasm:nobytes` de 2–3 `unity_*.obj` do build antigo e do novo (guardar 3 `.obj` antes do passo 6) e `fc`: só devem diferir nome do arquivo.
8. **O isolamento funciona** (simular uma edição de header, sem compilar): acrescentar um espaço em `include\runtime\ps2_vu1.h` e rodar `ninja -C ...\build_perf -n | find /c "Unity"` -> **0** unidades unity (antes: 1307); idem para `gs\gs_frontend.h`. Reverter a edição. (`-n` não executa; mas ainda assim só com o lock.)
9. Árvore de iteração: `cmake -S work\ps2recomp_game -B work\ps2recomp_game\out\build_dev -G Ninja -DCMAKE_MAKE_PROGRAM=<ninja VS> -DCMAKE_BUILD_TYPE=Release -DPS2X_RUNNER_OPT=dev -DPS2X_ENABLE_LTCG=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=ON -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_BUILD_RUNTIME=ON -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF` (mesmo conjunto de opções do `build_perf`, mais `PS2X_RUNNER_OPT=dev`). Primeiro teste: rodar o `menu` até o fim (stack) e comparar com uma referência **do dev** (ver riscos).
10. Promover para o versionado: copiar os arquivos novos para `patches/ps2xRuntime/` (`include/ps2_runner_prelude.h`, `include/ps2_runner_light_on.h`, `include/ps2_r5900_context.h`, `include/runtime/ps2_memory_map.h`, `tools/check_runner_prelude.py`) e regenerar `patches/ps2xRuntime-modified.patch` (as edições em `CMakeLists.txt`, `ps2_runtime.h`, `ps2_memory.h`, `ps2_address.h`, `ps2_stubs.h`, `ps2_runtime_macros.h`, `ps2_syscalls.h`, `ps2_recompiled_stubs.h` e `ps2xRecomp/src/lib/*.cpp`); atualizar `patches/README.md` (novas opções, e que **adicionar um método de `PS2Runtime` chamado pelo gerador exige declará-lo no prelude**). Regra 12: sem atribuição ao Claude nos commits.

## 7. Arquivos em `work/build_speedup/`

| Arquivo | Papel |
|---|---|
| `apply_build_speedup.py` | aplica/reverte o pacote (headers, CMake, gerador, include dos `.cpp` gerados); idempotente; faz backup; `--dry-run` |
| `make_light_headers.py`, `speedup_io.py` | extrai `ps2_r5900_context.h`/`ps2_memory_map.h` e ajusta os includes (preserva CRLF) |
| `ps2_runner_prelude.h`, `ps2_runner_light_on.h` | headers novos |
| `check_runner_prelude.py` | guarda de deriva prelude x `ps2_runtime.h` + cobertura dos `runtime->X` do runner |
| `build_speedup.patch` | o mesmo conjunto em diff unificado (**revisão**; gerado contra a árvore de hoje, ~970 linhas; use o script para aplicar, pois a árvore está em edição) |
| `parse_ninja_deps.py`, `analyze_fanin.py`, `ninja_log_stats.py` | análise de `.ninja_deps`/`.ninja_log` (fan-in, tempos) |
| `harness/` | `.bat`/`.ps1` das medições com `cl.exe` direto (`env.bat`, `memrun*.ps1`, `e*.bat`) |
| `scratch/` | cópias temporárias (headers, amostra do runner, `.obj`/`.pch`); **apagável** |

O `build_speedup.patch` e os scripts foram testados contra uma **cópia** da árvore (`scratch/applytest`): aplicação, reaplicação (idempotente), guarda do prelude (`0 divergencias`), e compilação do PCH final (macros + prelude + `light_on` via PCH, **sem** `-D`), de uma unidade com ele e de uma TU "completa" sem PCH (estilo `main.cpp`). **Não testado** [NAO MEDIDO]: `cmake` configure/`ninja` reais com o `CMakeLists.txt` editado (a árvore estava travada); portanto o bloco CMake (`SKIP_PRECOMPILE_HEADERS` em fontes que também estão em lotes unity, alvo `check_runner_prelude`) é revisão de código + diff, a validar no passo 6. O `.pch` leve também **não** foi medido no ninja real (o ganho de isolamento é pela análise de `.ninja_deps`/`build.ninja`: `unity_N.obj` depende de `cmake_pch.cxx.pch`).

## 8. Pendências / [NAO MEDIDO]

- Tempo de build completo real de cada alternativa (só amostras; sem a árvore ociosa).
- `-j` ótimo sob contenção; paginação (só inferida dos tempos do log).
- Lote do unity > 32.
- `build_dev`: velocidade do guest, pilha, divergência FP vs `build_perf`.
- `cmake`/`ninja` reais com o patch (passos 6–8).
- sccache com PCH (documentação, não testado).
- Dividir funções gigantes no gerador.

## 9. Aplicado em 05/11

Prelude leve aplicado em `work/ps2recomp_game` (backup em `work/build_speedup/backup_20261105_002712/`; `check_runner_prelude.py`: 29 declaracoes, 0 divergencias). Os 41 808 `.cpp` gerados foram **retargetados com o script** (troca do include no lugar, sem rodar o `ps2_recomp`; mesma troca ja esta nas 5 emissoes do gerador, entao uma regeneracao futura produz o mesmo). Os `.cpp` gerados nao sao versionados. Logs: `logs/build_full_prelude3_20261105.log`.

| Medida (maquina parada, `-j14`) | Antes | Depois |
|---|---|---|
| Editar 1 `.cpp` de `src/lib/gs` | 14,6 s | ~15 s (igual: ja era so o runtime + link) |
| Editar `ps2_vu1.h` / `gs_frontend.h` (header do fechamento do PCH) | PCH + 1307 unidades unity (~12 min) | **34,6 s** no total (touch de `ps2_vu1.h`, `gs_frontend.h` e `gs_frontend.cpp`: 42 passos, 0 unidades do runner, so ~39 `.cpp` do runtime + `ps2_runtime.lib` + link) |
| Rebuild completo (unico, ao aplicar) | ~12 min | ~12,5 min (00:38 -> 00:51, 1397 passos) |
| Identidade | | `regress.ps1 -Scenario menu`: byte-identico (md5 igual, exit 0) |

Contagem de unidades: antes, `ps2_vu1.h` estava no fechamento do `cmake_pch.cxx.obj` (que as 1307 unidades unity tem como dependencia implicita). Depois o PCH leve nao inclui `ps2_vu1.h`/`gs/*`/`ps2_runtime.h`; `ninja -n` nao serve para contar aqui (o dry-run para em "Re-running CMake" quando o `CMakeLists.txt` foi tocado), por isso a prova foi o rebuild real (0 unidades unity no log).

### Armadilhas encontradas
- **Reconfigure custa ~145 s** (geracao do `build.ninja` com 41 mil fontes): nao tocar `CMakeLists.txt` sem necessidade; qualquer fonte nova em `src/lib` listada explicitamente exige reconfigure.
- **Nunca configure com FetchContent online**: um `cmake` interrompido no meio do `git clone` do raylib apagou `_deps/raylib-src` e gerou um `build.ninja` sem include do raylib. Restaurado de `out/build/_deps/raylib-src`. Use `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` (o `work/cm.bat` ja faz).
- Arquivos novos em `src/lib` (ex.: `ps2_vu1_native.cpp`, `ps2_postfx_crt.cpp`) precisam estar na lista do `ps2xRuntime/CMakeLists.txt`, senao o link falha com LNK2019 (nao e erro do prelude). Se outro agente alterar o `CMakeLists.txt` a arvore `build_dev` so reconfigura quando a data do arquivo for mais nova que o `build.ninja`; na duvida rode `work/cm_dev.bat`.
- Adicionar metodo de `PS2Runtime` chamado pelo gerador: declarar em `ps2_runner_prelude.h`.

### Ferramentas
- `tools/build.ps1` (raiz, generico): `powershell -NoProfile -File ..\tools\build.ps1 [-BuildDir work\ps2recomp_game\out\build_dev] [-Target ps2EntryRunner] [-Jobs 14] [-DryRun] [-Explain] [-Recompact]` a partir de `ghost_pc/`. Usa o ninja do VS, pega `work\BUILD.lock.d` (espera ate 30 min), grava `logs/build_*.log`, imprime o tempo, e avisa se o `.ninja_log`/`.ninja_deps` encolheram ou se o ninja disse "starting over" (estado em `<build>\.build_ps1_state`).
- `tools/sync_patches.ps1` (em `ghost_pc/`): regenera `patches/` (ver `patches/README.md`).

### build_dev (runner em /Od)
Configurado com `work/cm_dev.bat` (mesmas opcoes do `build_perf` + `-DPS2X_RUNNER_OPT=dev`; semeado com `ThirdParty/` e raylib de `build_perf` para nao baixar nada). Rebuild completo medido: **~5,3 min** (00:57 -> 01:02, incluindo contencao com outros agentes compilando e ~2,5 min de configure). Ressalvas: nao medir FPS; `/Od` pode divergir em bits no FP (regress pode nao ser identico: gravar referencia propria dev-vs-dev); risco de pilha (subir `PS2X_STACK_RESERVE_MB` para 512 se `0xC00000FD`). **Pendente [PREENCHER]:** o link do `build_dev` ainda falhou em 05/11 01:12 por fontes WIP de outros agentes (`GSHwBackend` ainda fora do `CMakeLists.txt`; LNK2019), nao pelo prelude: todas as unidades do runner em `/Od` compilaram. Depois que a integracao do GS por hardware entrar no CMake, rode `work\cm_dev.bat` + `build.ps1 -BuildDir ...build_dev` e depois `regress.ps1 -Exe` com o exe dev (nao medido ainda).
