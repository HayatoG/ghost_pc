# Otimização do escalonador do IOP host (05/11/2026)

Motivação: o perfil `gameplay-profile-hw-2026-11-05.md` mostrou 22 % das amostras da thread do jogo no IOP host
(`IopKernel::beginNextReady` 17,9 % e `nextWakeCycle` 7,7 % do módulo), porque cada fatia de `IopEmulator::runCycles`
percorria o `std::map<int, IopThread>` inteiro duas vezes em `beginNextReady` e mais uma em `nextWakeCycle`.

## O que mudou (só `ps2xIOP/src/emulator/core/iop_kernel.{h,cpp}`; nenhum gs/, nenhum método novo de PS2Runtime, sem reconfigure do cmake)

1. **Caminho rápido para o ocioso**: `m_dirty` + `m_minWake`. Quando a última varredura não achou thread pronta, e nada
   mexeu no kernel desde então, e `currentCycle < m_minWake`, `beginNextReady` devolve `nullptr` sem varrer e
   `nextWakeCycle` devolve `min(fallback, m_minWake)` sem varrer. `m_dirty` é ligado na entrada de toda função pública que
   pode mudar estado de thread (`dispatchThread/Semaphore/EventImport`, `setInternalEventFlag`, `sleepCurrent`,
   `delayCurrentUntil`, `endTimeslice`, `cleanupDeadThreads`, `terminateThreadsInRange`, `reset`) e quando uma thread é
   escolhida. Só essas funções tocam o estado (verificado por grep: `iop_emulator.cpp` só usa a thread devolvida para `cpu`).
2. **Uma passada em vez de duas** quando precisa varrer: promoção Delay→Ready e escolha da melhor Ready na mesma
   iteração. Critério idêntico: menor `priority`, empate pelo menor `id` (a iteração é em ordem crescente de id).
3. **Vetor contíguo `m_order`** de ponteiros para as threads (nós de `std::map` são estáveis), reconstruído só quando o
   conjunto de threads muda (criação em `dispatchThreadImport`, remoção em `cleanupDeadThreads`/`reset`), em vez de
   caminhar pelo mapa (cache-miss por nó).

Não mexi em `IopTimrman` (array pequeno de timers; no perfil novo não aparece entre as funções quentes).

## Prova de identidade (oráculo de determinismo, `threading-design.md` seção 10)

`PS2X_GUEST_CLOCK_ONLY=1 PS2X_GS_STREAM_HASH=<arquivo>` + `pad_scripts/determinism_scene3d.txt`, mesma máquina, exe
antes (`work/iop_before/`, cópia do build_perf anterior) e depois (`work/iop_after/`):

| Item | Resultado |
|---|---|
| Arquivo de hashes por vsync (7 620 linhas) | `cmp` idêntico; md5 `35004f17b285988e2e4d62d0995e760c` (é o mesmo md5 do registro da seção 10.2) |
| Prefixo do `scene3d_guestclock/gs_stream_hash.txt` de referência | idêntico nas 7 620 linhas (a referência é do script completo, mais longo) |
| Capturas `d07564` / `d07600` | md5 idênticos antes/depois (`267f287b…`, `159bd2c4…`), iguais aos da seção 10.2 |

Como o IOP roda por ciclos do EE nesse modo, qualquer mudança de ordem de escalonamento mudaria o stream do GS; por isso o
oráculo cobre o escalonador e não precisei de hash de trace de despertares. No modo padrão (relógio do host) o IOP usa o
mesmo código; a lógica é a mesma e não depende do relógio. Não rodei o script completo `regress_scene3d` (6 capturas).

## Medição (hw, `work/pad_prof_hw.txt`, uma corrida cada, `tools/perf_run.ps1`; logs em `logs/iopopt/`)

Máquina compartilhada com outros agentes (carga ao iniciar: antes 36 %, depois 11-15 %), então os números absolutos são
ruidosos; a fração do perfil é o dado mais confiável.

| | antes | depois |
|---|---|---|
| Amostras do objeto `ps2_iop` na thread do jogo (% de todas as amostras) | 22,0 % (2 203/10 025) | **5,6 %** (567/10 025; 268 dessas são `applyRelocations`, carga de módulo, não escalonador) |
| `beginNextReady` + `nextWakeCycle` (% do módulo) | 25,6 % | fora do top 15 (< 1,9 %) |
| vsync/s, janela 7530-7640, sem amostrador | 11,55 | **13,76** |
| vsync/s, com amostrador (depois) | 11,61 (perfil de referência) | 15,54 |

Ganho: o custo do escalonador cai ~4x (22 % -> ~4-5 % da thread do jogo); +19 % de vsync/s na janela (11,55 -> 13,76),
com a ressalva de carga diferente entre as corridas. O "antes" do perfil é do documento do perfil (mesmo código, outro
binário; o `.map` do exe antigo foi sobrescrito, então não pude resolver símbolos do `iop_before`).

## Arquivos
- `work/ps2recomp_game/ps2xIOP/src/emulator/core/iop_kernel.h`, `iop_kernel.cpp` (rode `tools/sync_patches.ps1` para o patch)
- `work/oracle.ps1`, `work/measure_iop.ps1` (scripts descartáveis), `logs/gs_hash_iop_{before,after}.txt`, `logs/iopopt/*`
