# Contêiner `fpk` (IMAGE.BIN, BGM.BIN, RTDEN2EU.BIN, MOV*.BIN)

Verificado com `tools/fpk.py` (genérico) sobre os 7 arquivos do disco. Todos os offsets+tamanhos cabem no arquivo e não há sobreposição parcial (apenas aliases exatos). Nenhum byte do jogo está neste documento (só magics/tags).

Uso: `cd ghost_pc; python -I ../tools/fpk.py list --iso <iso> --lba N --size N [--into 1.3] [--json logs/x.json]` ou `list <arquivo.BIN>`; `extract ... --index N|--all --out <dir>`. Logs em `logs/fpk_<data>.log`, JSON em `logs/fpk_*.json`.

## Cabeçalho (little-endian, 0x80 bytes)

| Offset | Campo | Observação |
|---|---|---|
| 0x00 | magic `66 70 6b 00` ("fpk\0") | |
| 0x04 | 0 | |
| 0x08 | `count` | nº de entradas (IMAGE 0x1a6, BGM 0x100, RTDEN2EU 0x5d7, MOV00EUS 0x1ae, MOV00EU/MOV01EU 0x10, MOV02EU 0xb) |
| 0x0C | `align` | 0x800 nos arquivos de topo; **0x40** nos fpk aninhados |
| 0x10, 0x14 | 0x80 | início da tabela (usado como `table_off`); sentido do 2º igual não confirmado |
| 0x18 | `table_size` | >= count*16, arredondado (IMAGE 0x1a80 p/ 0x1a60 de entradas; BGM 0x1000). Não usado para parse |
| 0x1C..0x23 | 0 | |
| 0x24 | flag | 0 no topo, 1 no fpk aninhado observado (significado desconhecido) |
| 0x28 | `data_start` | início da área de dados = tabela alinhada a `align` (IMAGE 0x2000, BGM 0x1800, MOV 0x800; aninhado 0x100) |
| 0x2C | `total` | **tamanho da área de dados** (sem o cabeçalho): `data_start + total == tamanho do arquivo` nos 7 (no ISO o tamanho = esse valor, sem folga extra) |
| 0x40 | nome interno | string ASCII até 0x40 bytes (`image.bin`, `bgm.bin`, `mov00eu.bin`; aninhado: `pack.txt.bin`) |

## Tabela de entradas (em 0x80, `count` × 16 bytes)

`{u32 a; u32 off; u32 size; u32 tag}`

- `a` = 0 em todos os casos vistos (reservado).
- `off` é **relativo a `data_start`** (offset absoluto = `data_start + off`), alinhado a `align` (a hipótese inicial de offset absoluto estava errada: a 1ª entrada tem off 0).
- `size` em bytes; `size == 0` = slot vazio (ocorre em BGM, RTDEN2EU e IMAGE; `off` repete o da próxima entrada). Tamanhos não são alinhados; o próximo `off` = `align_up(off+size)`.
- Entradas podem ser **alias** (mesmo off/size): IMAGE 30, RTDEN2EU 57, MOV00EUS 215 (ex.: texto repetido por par de entradas).
- `tag` (w3) varia por tipo:
  - BGM/RTDEN2EU: sempre bytes `sdac` (provável extensão "cads" invertida; fonte: leitura do fourcc little-endian).
  - fpk aninhado: extensão em fourcc **invertido** (ex.: bytes `tsl\0` = "lst"; exts vistas: lst, dat, zim, pth, ico, wrd, txt).
  - IMAGE, entradas comprimidas (`V3a`): = tamanho **descomprimido** (bate 74/74 com o campo do cabeçalho de 8 bytes do bloco).
  - outros: 0 ou valor tipo-tamanho (não explicado; ex. entradas aninhadas com tag == tamanho de uma entrada do pai).
- **Não há tabela de nomes no topo.** Nos fpk aninhados (`pack.txt.bin`), a entrada 0 (ext `lst`) é um texto CRLF com um caminho por linha; a linha *i* nomeia a entrada *i* (verificado no primeiro pacote; os nomes são do jogo, não reproduzidos aqui). IMAGE tem entradas de 10 bytes com texto de marcador/versão ("cavia 2003") separando grupos (31 delas).

## Estatísticas por arquivo

| Arquivo | Entradas | Vazias | Únicas (não vazias, sem alias) | Conteúdo |
|---|---|---|---|---|
| IMAGE.BIN | 422 | 10 | 382 | 302 fpk aninhados, 74 blocos `V3a` comprimidos, 31 marcadores "cavia 2003" (10 B), 5 textos CRLF |
| BGM.BIN | 256 | 228 | 28 | 28 streams "cavia stream format v1.01" (cabeçalho de 32 B de nome + params) |
| RTDEN2EU.BIN | 1495 | 2 | 1436 | 1493 streams "cavia stream format v1.01" |
| MOV00EUS.BIN | 430 | 0 | 215 | texto TSV (frame inicial, duração, linha de legenda) em vários idiomas: provavelmente legendas dos MOV00EU; 40 entradas começam com dígitos de tempo |
| MOV00EU.BIN | 16 | 0 | 16 | 16 MPEG-PS (`00 00 01 BA`) |
| MOV01EU.BIN | 16 | 0 | 16 | 16 MPEG-PS |
| MOV02EU.BIN | 11 | 0 | 11 | 11 MPEG-PS |

Cada MPEG-PS tem `size % 0x800 == 4` e termina com a sequência de fim `00 00 01 B9` (os últimos 4 bytes são o end code; o resto do último setor é padding).

Dentro dos 302 fpk aninhados de IMAGE (todos validados, `align=0x40`): ~8 700 entradas, principalmente `zim` (6 100; magic `wZIM`), `pth` (520; magic `pBAX`), `dat` (221), `lst` (110), `ico` (20), `wrd`/`txt` (10 cada), mais 361 **fpk de 2º nível** (recursão: `--into 1.3` etc.). Formato `zim`/`pth`/`dat` não analisado.

## Compressão

- Contêiner em si não comprime nada. Entradas de IMAGE com início `00 56 33 61` ("\0V3a"; 74 no topo, 9 em aninhados) têm cabeçalho de 16 bytes: `+0 magic`, `+4 s32` = **número de blocos Huffman**, `+8 u32` = tamanho descomprimido (== `tag` da entrada), `+0xC u32` = tamanho comprimido (== `size` da entrada, inclui o cabeçalho). Soma: 180 094 371 -> 531 303 616 bytes (razão ~2,95).
- **Algoritmo identificado**: LZSS + Huffman estático, o decoder "ar002"/LHA `-lh5-` (NC=510, NT=19/TBIT=5, CBIT=9, NP=16/PBIT=4), classe `CHlzDecode` do ELF (`Hlz_Decode` em `0x00193fcc`). Implementado em `tools/v3a.py`; validado 74/74 no topo e 9/9 aninhados (tamanho exato; todas as saídas são fpk ou `wZIM` válidos). Detalhes em `docs/v3a-zim-format.md`.
- O decoder do jogo também aceita `\0V2` + u16 nblocks e 1º byte != 0 como nblocks (não vistos no disco).

## Incertezas / próximos passos

1. ~~Algoritmo `V3a`~~ resolvido (ver seção Compressão).
2. Campos 0x10/0x14/0x18/0x24 do cabeçalho e `tag` não-ASCII em entradas não comprimidas.
3. Ordem/identidade das entradas (nomes só existem nos `lst` aninhados; topo sem nomes). Mapear índice -> uso via xrefs no ELF (como o jogo abre `IMAGE.BIN`: provável acesso por índice + LBA base).
4. Formatos internos: `zim` (`wZIM`, textura?), `pth` (`pBAX`), `dat`, `ico`, `wrd`; stream "cavia stream format v1.01" (ADPCM? parâmetros no cabeçalho); legendas TSV do MOV00EUS (mapa entrada -> idioma/filme).
5. Se houver TIM2/SShd/VAG dentro, só aparecerão após descomprimir `V3a` ou abrir `zim`/`dat`: nenhum TIM2/VAG/SShd foi detectado nas entradas diretas.
