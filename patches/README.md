# Patches sobre o PS2Recomp (ps2xRuntime)

O PS2Recomp (GPL-3.0) fica em `third_party/` (fora do git). Nossas mudanças são mantidas aqui, reaplicáveis, para que a árvore de build (`work/ps2recomp_game/`, descartável) possa ser recriada.

| Item | O que é |
|---|---|
| `ps2xRuntime-modified.patch` | Diff dos arquivos existentes: `Pad.h/.cpp` (layout de botões compartilhado), `ps2_runtime.cpp` (FPS-alvo, pad virtual, medidor), `CMakeLists.txt` e `cmake/ReleaseMode.cmake` (opção `PS2X_ENABLE_LTCG`) |
| `ps2xRuntime/include/ps2_virtual_pad.h`, `src/lib/ps2_virtual_pad.cpp` | Gamepad virtual por script de quadros |
| `ps2xRuntime/include/ps2_frame_stats.h`, `src/lib/ps2_frame_stats.cpp` | Medidor de FPS (CSV por segundo + resumo com percentis) |
| `ps2xRuntime/tests/vpad_test.cpp` | Teste isolado do parser do pad virtual |
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

Com o relógio `guest`, o script avança com o jogo e não com a janela: uma captura cai no mesmo ponto do jogo seja qual for a velocidade do host ou do backend de GS (o script ainda é consultado uma vez por quadro do host, então cada passo vale no primeiro quadro do host cujo vsync é ≥ `<frame>`; o log `[pad] shot … (guest vsync N)` mostra onde caiu).

O resumo (`[perf] frames=… avg_fps=… one_percent_low_fps=… guest_vsync_hz=… realtime_pct=…`) sai no stdout ao encerrar. `realtime_pct` = ritmo do relógio emulado / ritmo esperado; 100% = velocidade real, abaixo disso o código recompilado está mais lento que o EE.

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
