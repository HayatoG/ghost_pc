# Guest com VU0 em modo macro: `ps2_vu0_update_mac` vetorizada e forçada inline (05/11/2026)

Objetivo: FPS máximo. Perfil de partida: `gameplay-profile-hw2-2026-11-05.md` (guest recompilado + VU0 macro = 17,1 % da thread do jogo; `FUN_001bf940` 5,6 %, `FUN_001bf590` 5,4 %, `ps2_vu0_update_mac` 2,8 % como função separada).
Sem commit (o líder commita). Nada de bytes do jogo aqui: só endereços, nomes nossos e números.

## 1. O que são as duas funções (lido do C++ gerado, `src/runner/FUN_001bf590_0x1bf590.cpp` e `FUN_001bf940_0x1bf940.cpp`)

| Função | Faixa | O que faz (pelo código) |
|---|---|---|
| `FUN_001bf590` | 0x1bf590-0x1bf7ac (~40 ops COP2) | Carrega um ponto (`lq`/`qmtc2`), subtrai dois vértices (`vf1`, `vf2`), faz produto vetorial (`vopmula`/`vopmsub`), quadrados das arestas e somas horizontais (`vaddx/y/z`), e decide por **bits de sinal/zero do MAC** lidos com `cfc2 $vi17` + `andi 0xCC/0xEE` + `bnez`. Devolve 0/1 em `$v0`: teste geométrico ponto/esfera contra triângulo (interpretação nossa; o padrão é de colisão) |
| `FUN_001bf940` | 0x1bf940-... (1429 linhas, 104 `ps2_vu0_update_mac`) | Laço sobre triângulos: `lqc2` dos vértices, normalização com `vrsqrt`/`vwaitq`/`vmulq`, produtos vetoriais, chama `FUN_001bf590` (`jal 0x1bf590`) e grava resultados (`sqc2`, `swc1`). Rotina de colisão contra malha (interpretação nossa) |

Ambas são uma instância do **padrão geral** do gerador: toda instrução COP2 aritmética vira
`{ res = PS2_Vxxx(...); vf[d] = blendv(vf[d], res, máscara); ps2_vu0_update_mac(ctx, res, dest); }`.
As operações de vetor já eram SSE inline (`ps2_vadd/vsub/vmul` + `ps2_vfinish`, caminho lento só com expoente 255; máscara por `blendv`; broadcast por `shuffle_ps`). O ponto fraco era só a atualização do MAC.

## 2. Causa do custo

- `ps2_vu0_update_mac` (`ps2xRuntime/include/ps2_runtime_macros.h`) era um laço escalar de 4 lanes com `memcpy` e 4 desvios dependentes de dados por lane. Era `static inline`, mas o MSVC **não a inlinava** dentro das funções gigantes geradas (orçamento de inline por função): aparecia como função própria no amostrador (2,75 % de toda a thread do jogo).
- Fora de linha, cada chamada recebe `ctx` por ponteiro: o compilador precisa materializar `ctx->pc` e os `vf[]` antes de cada uma e não pode eliminar a escrita de `vu0_mac_flags` que a instrução COP2 seguinte sobrescreve. Quase nenhum MAC é lido (em `FUN_001bf590`, 2 `cfc2` para ~20 atualizações).
- Flags "preguiçosas" no gerador (só materializar MAC quando `cfc2`/`vopmula`... lê) seriam a solução completa, mas mexem no ps2xRecomp e na regeneração dos 41 808 `.cpp`: fora do prazo. Com a função inline, o próprio otimizador faz isso: o MAC é só uma escrita em `ctx->vu0_mac_flags` (offset fixo de `ctx`), e as escritas mortas entre duas atualizações são removidas (dead store elimination), junto com o cálculo inteiro.

## 3. Mudança (rota A: helper do runtime; um rebuild completo do runner)

`ps2xRuntime/include/ps2_runtime_macros.h`:
- `ps2_vu0_update_mac` agora é **sem desvios, em SSE**, e `__forceinline`: inverte as lanes (`_mm_shuffle_epi32`) para que o bit i do `movemask` seja o componente i (x=8...w=1), e calcula S = sinal, Z = expoente 0, U = expoente 0 e magnitude != 0, O = expoente 255, com `mac = Z | S<<4 | U<<8 | O<<12`, mascarado por `dest * 0x1111`. Mesmas regras do laço original (que segue `VU_MAC_UPDATE` do PCSX2: zero negativo dá S, subnormal dá Z+U, Inf/NaN dá O, lanes inativas zeradas), por bits, independente de DAZ/FTZ do host e de `/fp:fast`.
- O laço antigo ficou como `ps2_vu0_update_mac_ref` (referência). `-DPS2X_VU0_MAC_SCALAR` na compilação volta ao laço antigo (rollback sem editar código).
- Rota B (override nativo das duas funções em `ghost_overrides.cpp`) descartada: 2 000 linhas de código gerado para reescrever e validar à mão em < 30 min, e o custo era de um padrão geral (vale para todas as funções com COP2 do jogo, não só estas duas).

## 4. Validação

| Teste | Resultado |
|---|---|
| `ps2xRuntime/tests/vu0_mac_test.cpp` (novo; inclui o header real, compilado com as mesmas flags do runner: `/O2 /arch:AVX2 /fp:fast`) | **OK, 32 331 776 comparações** nova x referência, todas idênticas: exaustivo em 12 classes por lane (±0, ±subnormal, ±normal, ±Inf, ±NaN, ±máximo normal) nas 4 lanes x 16 máscaras `dest`, mais 2 milhões de vetores aleatórios x 16 máscaras, com `vu0_mac_flags` inicial diferente nos dois lados |
| Equivalência das funções inteiras | Por construção: só a função de MAC mudou, e ela é bit a bit igual para toda entrada; os registradores VF/ACC/Q e a memória não passam por ela |
| Inlining de fato | O `.map` do exe novo (04:13:53) não tem mais nenhum símbolo `ps2_vu0_update_mac` (antes era função própria no perfil) |
| Oráculo de determinismo (`PS2X_GUEST_CLOCK_ONLY=1` + `PS2X_GS_STREAM_HASH`, `pad_scripts/determinism_scene3d.txt`) | base = cópia do exe de 02:51 (`work/vu0_base_exe/`) -> `logs/vu0_hash_base.txt`; novo = cópia do exe de 04:13 (`work/vu0_new_exe/`) -> `logs/vu0_hash_new.txt`. **Idênticos**: 7 620 linhas, md5 `35004f17b285988e2e4d62d0995e760c` nos dois (o mesmo md5 registrado em `threading-design.md` 10.2) |
| `regress.ps1 -Scenario menu` (exe novo) | **IGUAL (md5)** `84891ae3`, 0 falhas (`logs/vu0_menu_new.txt`) |
| `regress.ps1 -Scenario scene3d` (exe novo) | ver seção 6 |

Atenção: o rebuild completo que levou esta mudança foi disparado às 04:01 por outro agente (`build.ps1 -NoLock`, árvore `build_perf`, ninja iniciado 14 s depois da edição do header) e inclui também as edições dele. O meu `build.ps1` (que esperou o lock) terminou com `ninja: no work to do`, confirmando que o exe de 04:13 já tem a mudança. Como o hash saiu idêntico ao da base, nem esta mudança nem as outras do mesmo build alteraram o fluxo do GS no oráculo.

## 5. Ganho

Esperado (Amdahl sobre o perfil hw2): até 2,8 % da thread (a função some do perfil) mais parte do tempo próprio de `FUN_001bf940`/`FUN_001bf590` (escritas de `ctx` e cálculo de MAC mortos). [NAO MEDIDO] amostrador e vsync/s: o prazo (04:35, máquina parada para a medição do líder) não deixou janela de medição limpa depois do build. Medir com o método de `gameplay-profile-hw2-2026-11-05.md` (janela 7530-7640) e conferir que `ps2_vu0_update_mac` sumiu do top do `resolve_profile.py`.

## 6. Resultados do fim da sessão (04:20)

- `regress.ps1 -Scenario scene3d` com o exe novo: **6/6 IGUAL (md5)** (`q07300` ... `q07700`), processo em 177 s (`logs/vu0_scene3d_new.txt`).
- Menu IGUAL, oráculo idêntico (seção 4). Mudança mantida ligada (padrão); rollback = `-DPS2X_VU0_MAC_SCALAR`.
- vsync/s e amostrador: [NAO MEDIDO] (ver seção 5). O exe de 04:13 também tem as mudanças de outros agentes, então só um par A/B (`PS2X_VU0_MAC_SCALAR`) isola o ganho desta mudança; o perfil do host basta para conferir que `ps2_vu0_update_mac` (2,75 %) sumiu e quanto caiu o tempo próprio de `FUN_001bf940`/`FUN_001bf590`.
- Próximo passo, se o guest continuar alto: flags preguiçosas no gerador (ps2xRecomp: emitir a atualização do MAC só quando um `cfc2 $vi17`/`fsand`/`fmand`/`vopmula` seguinte puder ler antes de outra escrita) e cortar as escritas de `ctx->pc` por instrução nas funções sem chamadas.

Arquivos: `work/ps2recomp_game/ps2xRuntime/include/ps2_runtime_macros.h` (mudança), `work/ps2recomp_game/ps2xRuntime/tests/vu0_mac_test.cpp` (novo; o líder deve rodar `tools/sync_patches.ps1` para levar os dois para `patches/`). Logs: `logs/vu0_*`.
