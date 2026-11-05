# Código gerado do guest: custo de `ctx->pc`, MAC do VU0 e acesso à memória (05/11/2026, ~05:30)

Objetivo: FPS máximo. Pergunta: alguma redundância do código que o ps2xRecomp emite vale **>= 3 % da thread do jogo**? Regra da tarefa: só aplicar (rebuild completo de ~12,5 min, que bloqueia os outros agentes) se o ganho projetado passar disso.
Sem commit (o líder commita). Nada de bytes do jogo: só endereços, nomes nossos e números.

## 1. Decisão

**NÃO aplicado.** Ganho projetado **~2 a 2,5 % da thread do jogo** (abaixo dos 3 %), então não peguei o `BUILD.lock.d` nem disparei rebuild. Tudo pronto para entrar de carona no próximo rebuild completo que alguém fizer (ver seção 5): as duas mudanças são baratas, gerais e por construção não alteram o resultado.

## 2. O que o gerador emite (lido do C++ e do assembly do MSVC)

Fonte: `work/ps2recomp_game/ps2xRuntime/src/runner/FUN_001bf590_0x1bf590.cpp` compilado com as mesmas flags do runner (`/O2 /Ob2 /arch:AVX2 /fp:fast /GS- -DPS2X_RUNNER_LIGHT`, `/FAs`).

| Item | O que acontece | Achado |
|---|---|---|
| a) `ctx->pc = X;` por instrução | 1 `mov dword [rbx+512], imm` por instrução guest (162 no `bf590`, 436 no `bf940`); o MSVC não elimina nenhum (há chamadas ao runtime nos caminhos lentos) | barato: só uma escrita |
| b) MAC do VU0 | `ps2_vu0_update_mac` já é inline (sessão de 04:13), mas o MSVC **não remove nenhuma** atualização morta: 84 `vmovmskps` = 21 atualizações x 4, todas calculadas. STATUS/CLIP/sticky: o gerador **não calcula** (nada a estender) | 61 % das atualizações do jogo são mortas (8 543 de 13 891, em 889 arquivos; 75/104 no `bf940`, 12/21 no `bf590`) |
| c) acesso à memória (`READn`/`WRITEn`) | caminho rápido = `Ps2IsSpecialAddress` (kseg2/3 + 5 faixas: BIOS, scratchpad, IO, GS, VU) + máscara + checagem de contiguidade: ~12 instruções e 6-7 desvios por `lw`/`sd`, antes do acesso | dá para reduzir a 1 comparação para endereços físicos em RDRAM (`addr <= PS2_RAM_SIZE - n`) |
| GPRs | `SET_GPR_*` faz leitura-modifica-escrita de 128 bits em `ctx->r[]` (preserva os 64 bits altos): semântica, não redundância | não mexido |
| `eeCheckpointDue` em laço / `dispatchGuestBranch` | chamadas reais (definidas em `ps2_runtime.cpp`, área do agente EE) | fora do escopo |

## 3. Microbenchmark A/B (isolado, cl.exe direto)

`FUN_001bf590` (a rotina de teste ponto/triângulo do perfil) chamada 20 milhões de vezes com estado fixo, stubs para o runtime (nenhum caminho lento tomado: `slow=0`; resultado e soma de verificação iguais em todas as variantes). 3 repetições, mesma máquina.

| Variante | ns/chamada | vs. A |
|---|---|---|
| A: código atual | 66,6-68,5 | - |
| B: + caminho rápido de RDRAM nas macros | 60,0-62,7 | **-9 %** |
| C: A sem as escritas de `ctx->pc` | 64,6-68,4 | -1 a -3 % (ruído) |
| D: B sem `ctx->pc` | 56,4-59,8 | -13 % |
| G: B + MAC preguiçoso (passe abaixo), **com** `ctx->pc` | 56,4-57,6 | **-15,5 %** |
| F: G sem `ctx->pc` | 53,3-53,8 | -21 % |

Leitura: o `ctx->pc` vale ~2-4 % da função (não compensa regenerar 41 808 arquivos por ele agora); o caminho rápido de memória vale ~9 % e o MAC preguiçoso mais ~6-7 %.

## 4. Projeção (Amdahl sobre `gameplay-profile-hw2-2026-11-05.md`)

- `bf940` + `bf590` = 11,0 % da thread no perfil hw2, que é **anterior** ao MAC inline (então hoje são menos; não remedido). O `bf940` tem proporção maior de MAC morto (72 % x 57 %), então estimo -15 a -20 % nele.
- Projeção: ~9-11 % x 15-20 % = **1,4-2,2 %**, mais o caminho rápido no resto do guest (3,3 % x ~8-10 % = ~0,3 %) = **~1,7-2,5 % da thread**. Abaixo do limite de 3 %.
- `isSpecialAddress` fora de linha (0,9 % no perfil) vem do runtime (`ps2_memory.cpp`/EE, não do runner): estas macros não o afetam.

## 5. Pronto para aplicar (quando houver rebuild completo de qualquer forma)

1. **MAC preguiçoso**: `tools/lazy_vu0_mac.py` (novo, genérico). `cd ghost_pc; python -I ../tools/lazy_vu0_mac.py work/ps2recomp_game/ps2xRuntime/src/runner --apply` (sem `--apply` só conta). Precisa rodar de novo depois de cada `ps2_recomp`/`sync_recomp_output.py` (os gerados não são versionados); o lugar definitivo é o gerador (`vu_translation_helpers.cpp`/`code_generator.cpp`: emitir a atualização só se uma leitura do MAC puder vir antes da próxima escrita). Regras de segurança: barreira em rótulos, desvios, `return`, `runtime->`, outros `ps2_vu0_*`, qualquer menção a `vu0_mac_flags` (cfc2, vopmula/vopmsub) e `READ*`/`WRITE*`. O `FUN_001bf940` processado compilou sem erro.
2. **Caminho rápido de RDRAM** em `ps2xRuntime/include/ps2_runtime_macros.h`, uma linha no início de cada macro (antes de `isSpecialAddress`):
   - `READn`: `if (_addr <= PS2_RAM_SIZE - n) { T _v; std::memcpy(&_v, rdram + _addr, n); return _v; }`
   - `READ128`: `if (_addr < PS2_RAM_SIZE) return _mm_loadu_si128((const __m128i *)(rdram + _addr));`
   - `WRITEn`: `if (_addr <= PS2_RAM_SIZE - n) { const T _v = (T)(val); std::memcpy(rdram + _addr, &_v, n); } else` + o `if` original; `WRITE128` igual com `_mm_storeu_si128`.
   - Identidade: todas as faixas especiais (0x1000_0000 IO, 0x1100_0000 VU, 0x1200_0000 GS, 0x1FC0_0000 BIOS, 0x7000_0000 scratchpad, kseg2/3) ficam acima de `PS2_RAM_SIZE` (32 MiB), e para esses endereços o caminho antigo dava exatamente `rdram[addr]` contíguo (`ps2TraceGuestWrite` é vazio). Script de patch usado no banco: scratchpad `cg/patch_fast.py`.
3. Validar como sempre: oráculo `PS2X_GUEST_CLOCK_ONLY=1` + `PS2X_GS_STREAM_HASH` (7 620 linhas, md5 `35004f17b285988e2e4d62d0995e760c`), `regress.ps1 -Scenario menu` e `scene3d` 6/6, depois amostrador na janela 7530-7640.

Juntas, as duas são ~2 % da thread por um rebuild que outro agente já vá fazer; sozinhas não pagam os 12,5 min de máquina parada.

## 6. Próximos alvos maiores no guest

O guest inteiro é ~14 % da thread; mesmo zerando o overhead do gerador o teto é baixo. Ganhos maiores continuam em VU1 (23 %), runtime EE + espera ociosa (~21 %) e no lado CPU do GS (`gameplay-profile-hw2-2026-11-05.md`, seção 6).

Arquivos: `tools/lazy_vu0_mac.py` (novo). Banco A/B (fora do repo): `C:\Users\guilh\AppData\Local\Temp\claude\7ba1d17595f29844\f3f5e822-37f6-4d1d-9d25-9be11148fac6\scratchpad\cg\` (`bench.cpp`, `mk*.bat`, `incB/` com as macros alteradas, `lazy_mac.py`, `patch_fast.py`). Log: `ghost_pc/logs/lazy_vu0_mac.log`.
