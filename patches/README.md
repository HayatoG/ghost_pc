# Patches sobre o PS2Recomp (ps2xRuntime)

O PS2Recomp (GPL-3.0) fica em `third_party/` (fora do git). Nossas mudanças são mantidas aqui, reaplicáveis, para que a árvore de build (`work/ps2recomp_game/`, descartável) possa ser recriada.

| Item | O que é |
|---|---|
| `ps2xRuntime-modified.patch` | Diff dos arquivos existentes: `Pad.h/.cpp` (layout de botões compartilhado), `ps2_runtime.cpp` (FPS-alvo, pad virtual, medidor), `CMakeLists.txt` e `cmake/ReleaseMode.cmake` (opção `PS2X_ENABLE_LTCG`) |
| `ps2xRuntime/include/ps2_virtual_pad.h`, `src/lib/ps2_virtual_pad.cpp` | Gamepad virtual por script de quadros |
| `ps2xRuntime/include/ps2_frame_stats.h`, `src/lib/ps2_frame_stats.cpp` | Medidor de FPS (CSV por segundo + resumo com percentis) |
| `ps2xRuntime/tests/vpad_test.cpp` | Teste isolado do parser do pad virtual |
| `ps2xRuntime/include/ps2_runner_prelude.h`, `ps2_runner_light_on.h`, `ps2_r5900_context.h`, `include/runtime/ps2_memory_map.h`, `tools/check_runner_prelude.py` | **Prelude leve do runner** (05/11): o codigo gerado inclui `ps2_runner_prelude.h` (classe `PS2Runtime` enxuta + `R5900Context`) em vez de `ps2_runtime.h`; editar GS/VU1/audio/pad/runtime deixa de refazer os ~1300 lotes do runner. **Ao adicionar um metodo de `PS2Runtime` que o gerador chame, declare-o no prelude** (`check_runner_prelude` confere). Detalhes: `../docs/build-speedup.md` |
| `ps2xRuntime/include/runtime/gs/gs_banded_backend.h`, `gs_thread_backend.h`, `gs_command_queue.h`, `gs_backend_factory.h`, `gs_feature_stats.h`, `src/lib/gs/gs_banded_backend.cpp`, `gs_thread_backend.cpp`, `gs_backend_factory.cpp`, `gs_feature_stats.cpp` | GS em thread dedicada (`PS2X_GS=cpu-thread`), faixas paralelas (bands), fabrica de backends por ambiente e contadores de recursos |
| `ps2xRuntime/include/runtime/gs/gs_rhi_hw.h`, `src/lib/gs/gs_rhi_hw_d3d11.cpp`, `gs_hw_shaders.h`, `gs_gpu_shaders.h` | RHI/shaders do backend GS por hardware (D3D11) |
| `ps2xRuntime/src/lib/vu/ps2_vu1_access.h` (e `ps2_vu1_native.cpp`, quando existir) | Acesso do VU1 nativo ao interpretador (`PS2X_VU1_NATIVE`) |
| `ps2xRuntime/tests/gs_policy_test.cpp`, `tests/vu1_replay/` | Testes offline: politica do GS e harness de replay do VU1 (so codigo; gravacoes `.rec` nao entram) |
| `ps2xRuntime/include/ps2_irq_stats.h`, `ps2_sampling_profiler.h`, `src/lib/ps2_sampling_profiler.cpp`, `src/lib/Kernel/Stubs/GameLog.h` | Contadores de IRQ, profiler por amostragem, log do jogo |

**Gerado por `tools/sync_patches.ps1`** (nao edite o `.patch` e os arquivos novos a mao): ele refaz o `ps2xRuntime-modified.patch` (arquivos existentes no `third_party/PS2Recomp` vs `work/ps2recomp_game`: ps2xRuntime, ps2xRecomp, ps2xIOP, CMake), copia os arquivos novos de codigo (exclui `src/runner`, `ps2_recompiled_*`, binarios, `.rec`/`.png`), confere `git apply --check` e grava `logs/sync_patches_*.log`.
| `ps2xRuntime/include/runtime/gs/gs_rhi.h`, `src/lib/gs/gs_rhi_d3d11.cpp`, `include/runtime/gs/gs_gpu_backend.h`, `src/lib/gs/gs_gpu_backend.cpp` | Backend GS por GPU, etapa 1 (RHI D3D11 + `GSGpuBackend` que delega ao `GSCpuBackend` e apresenta pela GPU). **Ainda não ligado ao CMake/`gs_frontend.cpp`**: ver `gs_gpu_integration.txt` e `../../docs/gs-gpu-plan.md` |

`ps2xRuntime/src/lib/ghost_overrides.cpp` (arquivo novo): **overrides por endereço específicos deste jogo** (`SLES_530.20`), aplicados por `PS2_REGISTER_GAME_OVERRIDE`. Ligam funções internas do SDK que não têm HLE (`fioInit`, `fioIoctl`, `_sceCdSCmd`, `_sceCdNCmd`) a handlers genéricos (`ret0`/`ret1`). **Por que aqui e não no TOML do recompilador?** Acrescentar um stub no TOML muda `ps2_recompiled_stubs.h`, que todo arquivo gerado inclui — rebuild completo (~14 min). Um override é um arquivo só (~1 min). Para um novo "muro": uma linha em `kOverrides`.

`CMakeLists.txt` (no `.patch`) também reserva **256 MB de pilha** (`/STACK`, opção `PS2X_STACK_RESERVE_MB`): cada chamada do guest é uma chamada C++ aninhada, e a pilha padrão de 1 MB estourava (`0xC00000FD`) no código gerado.

`Kernel/EeScheduler.cpp` (no `.patch`): o período do vblank deixa de ser fixo em 60 Hz e passa a vir de `PS2X_VIDEO_HZ` (`include/ps2_video_clock.h`, compartilhado com o medidor).

## Variáveis de ambiente do runtime
| Variável | Efeito |
|---|---|
| `PS2X_TARGET_FPS` | Limite de apresentação do host. Padrão 60; `0` = ilimitado; ex. `30` |
| `PS2X_PAD_SCRIPT` | Caminho de um script do pad virtual (formato em `ps2_virtual_pad.h`; exemplo em `../pad_scripts/`) |
| `PS2X_PAD_CLOCK` | O que o `<frame>` do script conta: `guest` (padrão) = vsyncs emulados (`vsyncTick`, tempo de jogo) ou `host` = quadros apresentados do host (comportamento antigo). Os scripts antigos de `../pad_scripts/` foram escritos para `host`; `bench_menu_guest.txt` é a versão `guest` do benchmark do menu |
| `PS2X_FPS_LOG` | Caminho de um CSV (`t_s,fps,avg_ms,max_ms,guest_vsync_hz`, uma linha por segundo) |
| `PS2X_SHOT_DIR` | Pasta dos PNG do comando `shot` do pad virtual (padrão `shots/` no diretório corrente) |
| `PS2X_MISSING_LOG` | Arquivo onde o runtime acrescenta **cada alvo de desvio sem função gerada** (`0xALVO tipo source=0xORIGEM`, uma vez por alvo). Alimenta `tools/ghidra/CreateFunctionsAt.java` |
| `PS2X_HEARTBEAT` | Qualquer valor: imprime 1 linha por segundo (`[hb] frame= pc= ra= sp= dispfb1= display1= vsync= dma= gif= gsw= vif= threads=`) para ver **onde o jogo está** (preso esperando, desenhando, ocioso) |
| `PS2X_HEARTBEAT` (linhas `[hb-irq]`) | Com o heartbeat ligado, também imprime contadores das interrupções INTC/DMAC (`include/ps2_irq_stats.h`): `registered`, `dispatch`, `masked`, `not_recompiled`, `queued`, máscara habilitada e últimos valores. Distingue "o runtime nunca gera a interrupção" de "causa mascarada" e de "handler sem função gerada" (este último também vai para `PS2X_MISSING_LOG`) |
| `PS2X_VIDEO_HZ` | Taxa de vblank emulada: `60` (NTSC, padrão) ou `50` (PAL). O launcher define pela região do `game.toml` |
| `PS2X_GS` | *(integrado ao patch; ver `gs_gpu_integration.txt`)* `cpu` (padrão) ou `d3d11`. Extras: `PS2X_GS_VSYNC=1`, `PS2X_GS_DEBUG=1`, `PS2X_GS_FILTER=linear` |
| `PS2X_GS` (extras) | `cpu-thread` (alias `cpu-mt`): `GSCpuBackend` numa thread de GS dedicada. `PS2X_GS_QUEUE_KB` = capacidade da fila de comandos (KB) |
| `PS2X_GS_BANDS`, `PS2X_GS_BAND_ROWS` | Numero de faixas/trabalhadores e linhas por faixa do rasterizador paralelo (`gs_banded_backend`) |
| `PS2X_GS_BANDS_VERIFY` | `1` = compara as faixas com a referencia no Present; `2` = a cada draw |
| `PS2X_GS_GIF_STATE` | `1` liga o parser de GIF com estado (muda comportamento; padrao desligado) |
| `PS2X_GS_GPU_DRAW` | `0/off`: backend d3d11 so apresenta (etapa 1); padrao desenha na GPU |
| `PS2X_GS_WINDOW` | `0/off`: sem janela do d3d11 |
| `PS2X_GS_GPU_VERIFY` | Verificacao GPU x CPU (numero = modo/limite de quadros, ver `gs_gpu_backend.cpp`) |
| `PS2X_GS_GPU_MAXBATCH` | Limita o tamanho do lote de primitivas na GPU (bissecao) |
| `PS2X_GS_GPU_MASK` | Mascara de classes de desenho enviadas a GPU (hex/dec) |
| `PS2X_GS_GPU_HAZARD_LOG` | Nivel do log de hazards de ordem de escrita |
| `PS2X_GS_GPU_DOUBLE_Z` | `1`: Z em dupla precisao no shader |
| `PS2X_GS_GPU_FMA` | Modo de FMA no shader (numero) |
| `PS2X_GS_RECORD`, `PS2X_GS_RECORD_VSYNC`, `PS2X_GS_RECORD_PRESENTS` | Grava o fluxo de comandos do GS (arquivo, vsync inicial, no de presents) para replay offline |
| `PS2X_GS_FEATURES` | `1`: conta recursos do GS usados (`gs_feature_stats`) |
| `PS2X_VU1_NATIVE` | `1`: executor nativo de micro-programas VU1 |
| `PS2X_VU1_RECORD`, `PS2X_VU1_RECORD_VSYNC`, `PS2X_VU1_RECORD_COUNT` | Grava chamadas de micro-programa do VU1 (arquivo, vsync inicial, quantidade; padrao 4000) para `tests/vu1_replay` |

Com o relógio `guest`, o script avança com o jogo e não com a janela: uma captura cai no mesmo ponto do jogo seja qual for a velocidade do host ou do backend de GS (o script ainda é consultado uma vez por quadro do host, então cada passo vale no primeiro quadro do host cujo vsync é ≥ `<frame>`; o log `[pad] shot … (guest vsync N)` mostra onde caiu).

O resumo (`[perf] frames=… avg_fps=… one_percent_low_fps=… guest_vsync_hz=… realtime_pct=…`) sai no stdout ao encerrar. `realtime_pct` = ritmo do relógio emulado / ritmo esperado; 100% = velocidade real, abaixo disso o código recompilado está mais lento que o EE.

## Recompilador e `ghost.toml`
O patch também cobre `ps2xRecomp` (VF0 constante, MMI, FPU, VU) e `ps2xIOP` (`HeapBase`). Para gerar o C++, aplique o patch em `third_party/PS2Recomp/ps2xRecomp`, compile o `ps2_recomp`, rode e restaure a base com `git checkout -- ps2xRecomp`.

O `work/recomp/ghost.toml` não é versionado e precisa de `guest_prefixes = ["sceVu0", "cos", "sin", "tan", "log", "atan2", "sqrt", "__ieee754_", "__kernel_"]`, para essas funções rodarem do código do jogo em vez de stubs.

O patch não inclui `ps2xRuntime/src/runner`, que é C++ gerado a partir do jogo.

Rastreios de diagnóstico, ativados por variável de ambiente: `PS2X_GS_DUMP_VSYNC`, `PS2X_GS_DUMP_OUT`, `PS2X_GS_TRACE_PX`, `PS2X_GS_TRACE_PX_OUT`, `PS2X_MEMWATCH`, `PS2X_RPC_WATCH`, `PS2X_SEMA_TRACE`, `PS2X_GUEST_PROFILE`.

## Como reaplicar numa árvore nova
A partir da raiz do repo, com `third_party/PS2Recomp` já clonado (testado: o patch passa em `git apply --check` sobre o original e em `-R --check` sobre a árvore de trabalho):
```
robocopy third_party\PS2Recomp ghost_pc\work\ps2recomp_game /E /XD .git out android vita ps2xStudio ps2xTest
git apply --directory=ghost_pc/work/ps2recomp_game ghost_pc/patches/ps2xRuntime-modified.patch
robocopy ghost_pc\patches\ps2xRuntime ghost_pc\work\ps2recomp_game\ps2xRuntime /E /XF *.md
```
(`robocopy` retorna 1–3 em sucesso.)
(Depois, copiar o C++ gerado para `ps2xRuntime/src/runner` e `include`; ver `docs/ps2recomp-spike.md`.)

## Configurações de build usadas
| Build | Opções | Para quê |
|---|---|---|
| `out/build` (diagnóstico) | logs agressivos ON, LTCG ON | **Falhou no link** (`Stack overflow` no LTCG após ~1 h de CPU) |
| `out/build_perf` | `-DPS2X_ENABLE_LTCG=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF` | Medir FPS e otimizar |
