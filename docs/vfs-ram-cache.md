# Cache de arquivos do disco na RAM do host

Camada de cache **somente no host**: o guest continua vendo exatamente os mesmos bytes, posicoes e erros. Implementada em `ps2xRuntime/include/runtime/ps2_vfs_cache.h` (header-only, sem mudar CMake) e ligada em dois pontos de leitura.

## Onde o disco emulado e lido

Tres caminhos (o terceiro, `readHostRange`, esta descrito nas medicoes):

| Caminho | Quem usa | Antes | Agora |
|---|---|---|---|
| `PS2IopHostAdapter::openHostFile/readHostFile` (`ps2_iop_host.cpp`) | CDVD do IOP (`iop_cdvd.cpp`: `sceCdRead` por setor/offset em `IMAGE.BIN`, `BGM.BIN`, `MOV*.BIN`, modulos IRX) | `fopen` + `fseek` + `fread` sob mutex, por requisicao (setores de 2 KiB) | `VfsCache::readAt(id, offset, ...)` |
| `PS2Vfs::open/read/seek` (`ps2_vfs.cpp`) | `fio`/`open("cdrom0:...")` | `HostOpenFile` (`FILE*`) | `CachedOpenFile` (so `cdrom0:`, so leitura, so arquivo regular; posicao propria com a semantica de `fseek`/`fread`) |

Memory card, `host0:` e qualquer abertura com escrita **nao** passam pelo cache (podem mudar em tempo de execucao).
Cobre ISO e arquivos extraidos: o cache trabalha sobre o arquivo do host que o runtime ja resolveu (`IMAGE.BIN` etc. em `work/`).

## Design

- **Blocos** de 1 MiB (`PS2X_VFS_CACHE_BLOCK_KB`, 64..16384) por (arquivo, indice). Leitura de qualquer tamanho/offset e montada a partir dos blocos (cruza blocos; EOF devolve o que existe, offset alem do fim devolve 0 bytes sem erro, igual a `fseek`+`fread`).
- **LRU** global com orcamento em bytes; bloco em carga e compartilhado (varios leitores esperam o mesmo `condition_variable`, sem leitura duplicada). Falha de E/S no bloco nao e cacheada: o leitor cai para leitura direta do trecho (mesmo resultado de erro do caminho antigo).
- **Read-ahead**: se a leitura comeca onde a anterior do mesmo arquivo terminou (padrao sequencial), enfileira os proximos ate 4 blocos (limitado a 1/4 do orcamento em blocos) para uma thread de fundo.
- **Pre-carga**: ao abrir um arquivo com tamanho <= `PS2X_VFS_PRELOAD_MB` (padrao 16) e <= 1/4 do orcamento, todos os blocos sao enfileirados. Arquivos grandes (`IMAGE.BIN`, `BGM.BIN`) nunca sao pre-carregados inteiros.
- **Streams**: arquivos cujo nome comeca com um prefixo de `PS2X_VFS_CACHE_SKIP` (padrao `MOV`, ex.: `MOV00EU.BIN`) **nao** usam o cache (leitura direta, nao poluem a RAM com filme).
- **Thread-safety**: mutex de registro, mutex de blocos/LRU (nunca seguro durante E/S), mutex por arquivo apenas em volta do `fseek+fread`; fila do read-ahead com mutex proprio. Disco somente leitura, logo sem invalidacao.
- O `windows.h` fica so em `ps2_vfs.cpp` (`ps2x::vfsAvailablePhysicalBytes()`), porque `ThreadNaming.h`/raylib conflitam com ele nas outras unidades.

## Orcamento e variaveis de ambiente

| Variavel | Efeito |
|---|---|
| `PS2X_VFS_CACHE_MB=N` | Orcamento em MiB. `0` desliga (caminho antigo, `fopen/fread`). Ausente = **auto**: `min(25% da RAM fisica livre, 4 GiB)`, minimo 64 MiB |
| `PS2X_VFS_CACHE_LOG=1` | Estatisticas em stderr (`[vfs-cache] ...`) a cada ~10 s e ao encerrar: acertos, faltas, MiB servidos, MiB lidos do disco, MiB de bypass, blocos de read-ahead, evicoes, uso/orcamento, tempo de espera de leitura (ms) |
| `PS2X_VFS_CACHE_BLOCK_KB` | Tamanho do bloco (padrao 1024) |
| `PS2X_VFS_PRELOAD_MB` | Limite de pre-carga de arquivos inteiros (padrao 16; 0 desliga) |
| `PS2X_VFS_CACHE_SKIP=a,b` | Prefixos (sem diferenciar maiusculas) de arquivos em stream; padrao `MOV` |

Maquina com 8 GB de RAM e ~4 GB livres -> ~1 GiB de cache; com pouca RAM livre, o piso de 64 MiB mantem so read-ahead util. O orcamento e calculado uma vez no primeiro uso, nunca ultrapassa 25% do que estava livre.

## Verificacao de correcao

`ps2xRuntime/tests/vfs_cache_test.cpp` (compila isolado com `cl.exe`, gera arquivo sintetico aleatorio de ~5 MiB; nada do jogo): compara byte a byte com o conteudo de referencia em 400 leituras aleatorias (offset/tamanho arbitrarios, inclusive alem do EOF e tamanho 0), varredura sequencial por setor de 2 KiB, leituras na fronteira de bloco, 4 threads concorrentes, e tres configuracoes (cache desligado, orcamento de 3 blocos forcando evicao, orcamento folgado com pre-carga). Resultado: `OK` nas tres; uso nunca excede orcamento + 1 bloco.

## Medicoes

Build `build_perf` com o cache; `regress.ps1 -Scenario menu`: **IGUAL (md5)**, 0 falhas. Cenario `regress_scene3d.txt` (6 capturas) uma vez com `PS2X_VFS_CACHE_MB=0` e uma com o cache automatico (orcamento 4096 MiB): as 6 capturas tem os mesmos MD5 nas duas execucoes.

| Execucao | Tempo de parede (scene3d) | Estatisticas do cache |
|---|---|---|
| sem cache (`MB=0`) | 180,9 s | n/a |
| com cache (auto, 4 GiB) | 179,4 s | hits=11 misses=36, servido 16 MiB, lido do disco 96 MiB, bypass (MOV) 65 MiB, espera de leitura 66,8 ms |

**Ganho medido: nenhum perceptivel (~0,8%, dentro do ruido).** Neste cenario o tempo e dominado pela emulacao (relogio por vsync de guest com FPS ilimitado), e a E/S total e pequena (espera de leitura de 67 ms no total). O cache ainda nao tem como mostrar ganho aqui; o beneficio esperado fica para discos lentos (HDD, rede) e telas de carregamento reais com arquivos grandes. O custo e leitura em blocos de 1 MiB: 96 MiB lidos do disco para 16 MiB servidos (amplificacao ~6x em leituras pequenas e esparsas; reduzivel com `PS2X_VFS_CACHE_BLOCK_KB` menor).

Alem dos dois pontos da tabela acima, o caminho real do `sceCdRead` do EE (`readHostRange` em `Kernel/Stubs/Helpers/Support.h`, que antes abria um `ifstream` a cada leitura) tambem passa pelo cache; o resto zerado em leitura curta e os codigos de erro foram preservados.

## Riscos e limites

- O cache duplica na RAM do host o que o SO ja guarda no cache de arquivos; o ganho esperado vem de (a) menos syscalls/seeks por setor de 2 KiB, (b) read-ahead em thread de fundo, (c) menos contencao do mutex do host adapter. Em maquinas com SSD e arquivos ja quentes no SO, o ganho pode ser pequeno.
- Leituras diretas com falha de E/S em bloco refazem a leitura do trecho (sem cache); nao ha repeticao automatica.
- O prefixo `MOV` como padrao de stream e heuristica de nome (configuravel pela env); um jogo com outro nome de filme precisa ajustar `PS2X_VFS_CACHE_SKIP`.
- Arquivos tocados: `ps2_vfs_cache.h` (novo), `ps2_vfs.cpp`, `ps2_iop_host.cpp`, `Kernel/Stubs/Helpers/Support.h`, `tests/vfs_cache_test.cpp` (novo). Eles precisam entrar em `tools/sync_patches.ps1` (rodar o script) e na tabela de `patches/README.md`.
