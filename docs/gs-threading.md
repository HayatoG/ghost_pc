# Pipeline entre threads e frontend do GS (2026-11-04)

Escopo: `gs_thread_backend.*`, `gs_command_queue.h`, `gs_banded_backend.*` (novo), `gs_frontend.*` (diagnóstico GIF).
Base: `gameplay-profile-2026-11-04.md` (cena 3D ~2,7 vsync/s; `cpu-thread` com produtor ~44 % bloqueado).

## 1. Por que o produtor ficava bloqueado (diagnóstico)

No perfil, ~400 mil esperas de 0,011 ms por 10 s: o produtor não esperava "a fila encher", esperava **uma vaga**.
A fila antiga acordava o produtor a cada item consumido (`notify_all` por pop); ele empurrava 1 comando, a fila
voltava a ficar cheia, e dormia de novo: ping-pong de threads (2 trocas de contexto por comando), não paralelismo.
Além disso 1 MiB = ~3 000 comandos = ~0,15 quadro de gameplay (21,7 mil comandos/quadro, ~6,7 MB), pouco para
cobrir a diferença de ritmo entre o guest (em rajadas) e o rasterizador.

## 2. Mudanças (gs_command_queue.h, gs_thread_backend.*)

- **Histerese**: o produtor bloqueado só acorda quando o backlog cai a 50 % da capacidade (ou a fila esvazia).
- **Consumo em lote**: `PopBatch` troca (`swap`) todo o backlog por uma fila local; a GS thread executa sem tocar
  no mutex e reporta progresso (`Release`) a cada 64 comandos ou logo após comandos que alguém pode estar esperando
  (`CallCmd`, `FlushCmd`, `QuitCmd`). Bytes só são liberados no `Release` (o backpressure enxerga o trabalho em andamento).
- `notify` só se há alguém dormindo (flags/contadores sob o mutex).
- Fila padrão **32 MiB** (era 1 MiB); `PS2X_GS_QUEUE_KB` continua valendo.
- Não fiz fila lock-free SPSC: a taxa é ~65-100 mil comandos/s, o mutex custa microssegundos por segundo; o problema era o ritmo
  de acordar, não o custo do lock. Há também um segundo produtor (thread da janela chama `Flush`/`Present`), então seria MPSC.

### Medição (cena de gameplay, `scene3d.txt`, `PS2X_GS=cpu-thread PS2X_GS_DEBUG=1`, exe direto; máquina carregada por 5 agentes, só valem as razões)

| Fila | esperas do produtor / 10 s | GS thread ociosa / 10 s | comandos / 10 s | `guest_vsync_hz` (corrida inteira) |
|---|---|---|---|---|
| 1 MiB antiga (corrida F do perfil) | ~400 000 (x 0,011 ms) | 3,4-4,0 s | ~650 mil | 29,2 (perfil, máquina livre) |
| 1 MiB + histerese + lote | ~280 (x ~16 ms = 4,6 s) | 3,6-4,0 s | ~640 mil | 26,4 |
| 32 MiB + histerese + lote | 3-13 (esperas longas) | **0** (sempre ocupada) | 0,8-1,25 milhão | **33,2** |

Leitura: com 32 MiB a thread de GS deixa de ficar ociosa e processa +55-90 % mais comandos por janela; o gargalo vira o
rasterizador em si (100 % ocupado), como previsto pela análise (teto = o mais lento dos dois). Ganho de corrida inteira +26 %.
Custo: `Present` (thread da janela) espera o backlog drenar: 0,3-0,9 s na gameplay (era ~20 ms). Isso só atrasa a exibição,
não o jogo (o relógio do pad script é o do guest). Para menos latência: `PS2X_GS_QUEUE_KB=8192`.
`[NAO MEDIDO]` 8 MiB e 16 MiB.

## 3. Rasterizador em faixas (`PS2X_GS_BANDS=N`, `PS2X_GS_BAND_ROWS=32`) - experimental, desligado

`gs_banded_backend.*`: um `GSRasterBackend` que roda dentro da GS thread (despachante) e fala com N threads de trabalho,
cada uma com seu `GSCpuBackend` sobre a **mesma VRAM** (`gs_cpu_backend.*` nao foi alterado). Ativado so em `cpu-thread`.
- Tela em faixas de 32 linhas; a faixa `s` pertence ao worker `s % N`. Cada draw vai (em ordem) aos workers cujas faixas toca; cada
  worker chama `Submit` uma vez por faixa sua com o **scissor y recortado a faixa** (o rasterizador calcula o pixel so de coordenadas
  absolutas e do scissor: a saida deveria ser identica). Pixels de linhas diferentes nao dividem bytes (CT32/24/16, Z).
- **Barreiras** (todos os workers drenam; depois o cache de texturas de cada um e invalidado): textura lida de regiao escrita antes
  (RAW), escrita sobre regiao ja *lida* por um draw anterior (WAR; foi o que o verificador achou: sem isso sai diferenca), escrita
  sobre regiao escrita com outro layout, `LoadClut` sobre regiao suja, lista de regioes cheia, e todo comando que nao seja draw/CLUT.
  Draw que amostra a propria area de destino (copia de tela) roda serial no backend mestre.
- Lotes: ate 128 itens por worker; `FlushPending` antes de a GS thread bloquear.
- Diagnostico: `PS2X_GS_DEBUG=1` mostra `bands.draws/barriers/hazard_barriers(layout, full, tex, self, war)`.
- **Verificador** `PS2X_GS_BANDS_VERIFY=1`: um `GSCpuBackend` serial "sombra", com VRAM propria, recebe o mesmo fluxo de comandos; em
  cada `Present` as duas VRAMs sao comparadas (`[gs-bands] VERIFY MISMATCH`). `=2` compara apos cada draw (serializa os workers e
  esconde corridas; so para localizar o draw culpado). Independe do relogio do shot (que com GS assincrono nao e deterministico).
- Resultados: menu com bands=4 e verificador: 0 divergencias (imagem identica a referencia). Gameplay: **ainda com divergencias**
  (primeiro quadro 3D, fbp=160, regiao nao desenhada pelos workers) e **sem ganho**: a maioria dos draws cai em barreira
  (`self` e `layout` altos) porque a estimativa de extensao das areas escritas/lidas era grande demais (Z16S estimado com paginas de
  32 linhas; corrigido para altura de pagina por PSM, **ainda nao medido**). **Nao usar** `PS2X_GS_BANDS` ate o verificador passar
  em `scene3d` e as barreiras caírem; o padrao e `bands=0`. Requer `gs_banded_backend.cpp` no `CMakeLists.txt` (ja na arvore de trabalho).

## 4. GIF: `processGIFPacket` nao guarda estado entre pacotes

Verificado e **medido** (`PS2X_GS_DEBUG=1` conta `[gs:gif] truncated tag`): no menu 0 ocorrencias; na gameplay 3D, **sim**, em rajadas
(ex. pacotes de 96/64/80 bytes com tag IMAGE nloop=64 e depois "tags" de lixo como flg=0 nloop=767: dados de imagem lidos como tag).
O parser original, ao ver um tag que pede mais payload que o pacote, descarta o resto, e o pacote seguinte e lido como se
comecasse num tag. O hardware continua o tag no pacote seguinte (por caminho PATH1/2/3).

Implementei o parser com estado por caminho (`GifStreamState`, caminho vindo de `GifArbiter::currentPath()`, thread_local; XGKICK do VU1
entra direto como PATH1). **E uma mudanca intencional de comportamento: muda o que e desenhado na gameplay** (a regressao `scene3d`
passou a diferir do baseline a partir de q07564 com ele ligado; desligado, `scene3d` volta a ser byte-identico: `regress.ps1 -Scenario
scene3d` = OK, 6/6, as 23:01). Por isso fica **desligado por padrao**; ligar com `PS2X_GS_GIF_STATE=1` ate validar contra o PCSX2.
O menu nao muda em nenhum dos modos.

## 5. `launcher.cfg` sobrescreve `PS2X_GS`

`launcher-rs/src/run.rs::build_env` insere `PS2X_GS` sempre que `renderer` tem valor, e o `launcher.cfg` tem `renderer = cpu`;
`--renderer` só aceita `cpu`/`d3d11`. Correção sugerida (fora do meu escopo): aceitar `cpu-thread` em `--renderer`/`renderer`
(e `PS2X_GS_BANDS`/`PS2X_GS_QUEUE_KB`), ou só definir `PS2X_GS` quando o ambiente do usuário não o traz.
Enquanto isso, testes de `cpu-thread` foram feitos chamando `ps2EntryRunner.exe` direto (`logs/gt/run.sh`).
