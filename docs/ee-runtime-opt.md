# Corte do overhead do runtime EE (05/11/2026)

## Causa da espera ociosa
`EeScheduler::waitForEvent` (e `processDueDeadlines`) dormiam até o `hostDeadline` do próximo evento escolhido por ciclos. O
vblank tem prazo de host encadeado (atrasado quando o jogo é lento, então não dorme), mas alarmes e o timer do EE usam
`now() + duração`, sempre no futuro: mesmo com o jogo atrás do tempo real, o EE ocioso dormia em tempo real até eles.

## Mudanças (comportamento preservado quando o host é mais rápido que o jogo)
| Arquivo | Mudança |
|---|---|
| `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` | `waitForEvent` e `processDueDeadlines`: se algum `VBlankStart` pendente já venceu no relógio do host (jogo atrás do ritmo PS2X_VIDEO_HZ), trata prazos de host como alcançados (mesmo caminho do `PS2X_GUEST_CLOCK_ONLY`: pula o ocioso em ciclos). Quando o jogo está adiantado nada muda: continua dormindo e limitado a 50/60 Hz. `steady_clock::now()` só é chamado quando necessário |
| `ps2xRuntime/src/lib/ps2_memory.cpp` | `advanceEeTimers`: o `unordered_map::find` de GIF_STAT por chamada virou slot em cache (nós estáveis; invalidado no `clear()` do init). Semântica idêntica (a limpeza do FQC continua a cada chamada) |

Não feito (sem tempo): lazy dos timers, atomics em locks, QPC/heap por evento (varredura de chamadores não concluída).

## Validação (exe antes = cópia em `work/ee_before/`, depois = `work/ee_after/`)
- Oráculo (`PS2X_GUEST_CLOCK_ONLY=1` + `PS2X_GS_STREAM_HASH`, `determinism_scene3d.txt`): antes e depois **idênticos**, 7620 linhas, md5 `35004f17...` (igual à referência).
- `regress menu`: byte a byte igual (md5 `84891ae3...`).
- vsync/s na janela 7530-7640 (`PS2X_GS=hw`, `TARGET_FPS=0`, 1 corrida cada, com 2 outros `ps2EntryRunner` na máquina nas duas): antes **18,7**, depois **22,3** (+~20 %, ruidoso; precisa repetir com a máquina parada).
- Build: reconstrução total de 1323 passos (headers de outros agentes), 748 s; log `logs/ee_opt_build.log`.
