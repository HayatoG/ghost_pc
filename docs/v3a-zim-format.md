# Compressão `V3a` e texturas `wZIM` (zim); notas sobre `pBAX` e `pth`

Ferramentas genéricas: `tools/v3a.py` (descompressor + validação sobre um fpk) e `tools/zim.py` (info + PNG, sem Pillow). Neste documento não há bytes, pixels nem nomes de arquivos do jogo: só estrutura, endereços do ELF e contagens. As amostras extraídas ficam em `work/` (não versionado).

```
cd ghost_pc
python -I ../tools/v3a.py validate --iso <iso> --lba 2079641 --size 314607616 [--out-dir work/v3a_dec] [--json logs/v3a_validate_IMAGE.json]
python -I ../tools/v3a.py decompress <bloco.bin> <saida>
python -I ../tools/zim.py info   --iso <iso> --lba 2079641 --size 314607616 --path 4.16
python -I ../tools/zim.py to-png --iso <iso> --lba 2079641 --size 314607616 --path 4.16 work/sample/x.png
```
`--path` segue índices de fpk aninhados separados por ponto (ex.: `414.2.574`); um bloco `V3a` no caminho é descomprimido automaticamente. Logs: `logs/v3a_<data>.log`, `logs/zim_<data>.log`; resultado da validação em `logs/v3a_validate_IMAGE.json`.

## 1. `V3a`: LZSS + Huffman estático (decoder "ar002" / LHA `-lh5-`)

### Evidência no ELF (`/SLES_530.20`)

Classe C++ `CHlzDecode` / `CHlzDecodeSpad` (nomes vindos da RTTI do GCC: strings `10CHlzDecode`, `14CHlzDecodeSpad`; funções de typeinfo `FUN_0030a574` e `FUN_0030a528`; vtable de `CHlzDecodeSpad` em `0x00422e90`). "Hlz" = Huffman + LZ.

| Endereço | Papel (nome nosso) | Observação |
|---|---|---|
| `0x00193fb8` | `Hlz_Init(obj, src, dst)` | guarda ponteiros de entrada/saída |
| `0x00193fcc` | `Hlz_Decode(obj, budget)` | lê o cabeçalho, laço de símbolos; com `budget != 0` para quando o timer (`FUN_00100508`) passa do limite e retoma depois (descompressão incremental). Retorna bytes escritos ao terminar, 0 se pausou, <0 em erro (-1 magic, -2 versão) |
| `0x001935e0` | `init_getbits` | bitbuf de 16 bits, `fillbuf(16)` |
| `0x00193c3c` / `0x00193bfc` | `fillbuf(n)` / `getbits(n)` | MSB primeiro, byte a byte |
| `0x0019361c` | `decode_c` | lê novo bloco quando `blocksize == 0`; tabela de 12 bits + árvore |
| `0x0019373c` | `decode_p` | tabela de 8 bits + árvore; `p = (1<<(j-1)) + getbits(j-1)` |
| `0x00193808` | `read_pt_len(n, nbit, i_special)` | chamado com (19,5,3) e (16,4,-1) |
| `0x00193a04` | `read_c_len` | NC = 510, CBIT = 9 |
| `0x00193cd8` | `make_table` | canônico; tabela inválida -> flag de erro 2 |
| `0x00194244` / `0x001942b8` | métodos virtuais da `Spad` | copiam o estado de 0x3480 bytes (tabelas) entre o objeto e o scratchpad |
| `0x00252084` | um chamador | instância global em `0x005bb270`, chama `Hlz_Decode(obj, 500)` em fatias |

A estrutura é, função por função, o decoder `ar002` de Haruhiko Okumura (o mesmo do LHA `-lh5-`): mesmas constantes (NC=510, NT=19, TBIT=5, CBIT=9), mesmo tratamento de `c == 7` em `read_pt_len`, mesmas regras 0/1/2 para zeros em `read_c_len`, mesmo `make_table` com `left/right` compartilhados.

### Formato do bloco

| Offset | Campo |
|---|---|
| +0x0 | `00 'V' '3' 'a'` |
| +0x4 | `s32 nblocks` = número de blocos Huffman (o decoder decrementa por bloco; ao ficar < 0 termina com sucesso). Ex.: 2, 5, 9, 14 … 404 |
| +0x8 | `u32` tamanho descomprimido (== `tag` da entrada no fpk) |
| +0xC | `u32` tamanho comprimido incluindo o cabeçalho (== `size` da entrada) |
| +0x10 | bitstream |

O decoder aceita ainda dois cabeçalhos mais antigos: `00 'V' '2' u16 nblocks` (bitstream em +5) e "1º byte != 0" = `nblocks` em 1 byte (bitstream em +1). Nenhum dos dois foi visto no disco; `v3a.py` os suporta.

Cada bloco: `u16 blocksize` (nº de símbolos), `read_pt_len(19,5,3)`, `read_c_len()`, `read_pt_len(16,4,-1)`, depois `blocksize` símbolos:
- `c < 256`: literal;
- `c >= 256`: cópia de `c - 253` bytes (3..256) da distância `decode_p() + 1` para trás no **próprio buffer de saída** (sem janela circular; NP=16 com PBIT=4 limita a distância a < 2^15 na prática).

Diferenças em relação ao LHA: não há CRC nem tamanho final no stream; o fim é dado por `nblocks`. PBIT=4 com NP=16 é uma combinação que o LHA não usa (lh5 usa NP=14/PBIT=4; lh6 usa NP=16/PBIT=5).

### Validação

- **74/74** entradas `V3a` do topo de IMAGE.BIN: saída == campo +8 (total 180 094 371 -> 531 303 616 bytes). Todas as 74 saídas começam com `fpk\0` e são fpk válidos para `tools/fpk.py` (0 problemas de validação em toda a árvore).
- **9/9** blocos `V3a` dentro de fpk aninhados (IMAGE 413/414/415): tamanho correto; saídas = fpk ou `wZIM`.
- Tempo em Python puro: ~0,3–0,5 s/MB de saída (tudo em ~1,5 min).
- Os fpk descomprimidos usam `align` 0x10 e tipos novos (ex.: `plp`, `bin`, `csf`, `mpk`, `spk`, `chf`, `cvf`, `hi4` …; contagem em `logs/v3a_*.log`).

## 2. `wZIM` (zim): textura pronta para o GS

Encontradas 10 445 texturas (topo + aninhados + dentro dos `V3a`). **Todas** parseiam e satisfazem `size == w*h*bpp/8`. PSM: 6 457 PSMT4 (16 cores) e 3 982 PSMT8 (256 cores) + 6 arquivos com 2–4 imagens; paleta sempre CT32 (CPSM=0), CSM1. Nenhum PSMCT32/16 visto (tool tem caminho para CT32, não testado).

O ELF reconhece o magic comparando a string `wZIMd` (`wZIM` + versão `0x64` = 'd') em `FUN_001da774`, `FUN_001fb824`, `FUN_0024546c`, `FUN_001d0a14`, `FUN_001e4038` (não decompiladas a fundo).

### Cabeçalho (LE)

| Offset | Campo |
|---|---|
| 0x00 | `wZIM` |
| 0x04 | versão = 100 (0x64) |
| 0x08 | `nimg` (nº de imagens) |
| 0x0C | `npal` (nº de paletas; == nimg em todos os casos) |
| 0x10 | offset dos registros de imagem (0x20) |
| 0x14 | offset dos registros de paleta (= 0x20 + 0x28·nimg) |
| 0x18 | offset da cadeia DMA (registros de paleta alinhados a 0x40) |
| 0x1C | 0 |
| 0x20 | offset do 1º pacote GIF (fim da cadeia DMA) |

### Registro de imagem (0x28 bytes)

| Offset | Campo |
|---|---|
| +0x00 | `u32` offset do pacote de upload da imagem |
| +0x04 | `u32` índice (0..n-1; usado como índice da paleta) |
| +0x08 | `u16` nº de blocos GS ocupados (PSMT4: blocos de 32×16; PSMT8: 16×16) |
| +0x0A | `u16` nº de páginas GS (PSMT4: 128×128; PSMT8: 128×64) |
| +0x0C | `u16` largura, `u16` altura (reais, não precisam ser potência de 2: ex. 24×16, 160×32, 424×96, 648×456) |
| +0x10 | 0 |
| +0x14 | `u32` tamanho dos pixels em bytes |
| +0x18 | `u64` **TEX0** do GS (PSM, TBW, TW/TH = log2 arredondado, TCC=1, CLD=1; TBP0/CBP relativos, ajustados em runtime) |
| +0x20 | `u64` **TEX1** (0x260 = filtro bilinear MMAG/MMIN, sem mip) |

### Registro de paleta (0x10 bytes)

`u32` offset do pacote de upload, `u32` tamanho (64 = 16 cores, 1024 = 256 cores), `u64` molde de TEX0 do CLUT (CBP/CLD/PSM do índice).

### Cadeia DMA e pacotes

Na seção em 0x18 há DMAtags `call` (ID 5) apontando para cada pacote e `next`/fim (ID 2) — duas listas (provavelmente uma por imagem e uma para upload conjunto). Cada pacote: DMAtag `cnt` + VIF `DIRECT` com GIFtag A+D (`BITBLTBUF` com DBP placeholder `0xcccccccc` preenchido em runtime, DPSM = PSM, `TRXPOS`, `TRXREG` = w×h, `TRXDIR`), depois DMAtag `ret` + VIF `DIRECT(nloop+1)` + GIFtag `FLG=IMAGE` com `nloop` qwords de pixels. Com um único GIFtag os dados ficam em `pacote + 0xA0`; `zim.py` procura os GIFtags IMAGE (precedidos do `DIRECT` correspondente) e concatena.

### Pixels e paleta

- Os pixels estão **lineares** (ordem de linhas, sem swizzle no arquivo): o GS faz o swizzle na transferência porque DPSM = PSMT4/PSMT8. PSMT4: nibble baixo = pixel da esquerda.
- Paleta CT32 RGBA; PSMT8 usa a permutação CSM1 (entradas 8–15 <-> 16–23 em cada grupo de 32). Paleta de 16 cores enviada como retângulo 8×2.
- Alfa: escala do PS2 (0x80 = opaco). `zim.py --alpha ps2` (padrão) dobra e satura em 255. Há paletas com alfa até 0xFF (que o GS trata como >1); a escala exata usada pelo jogo (TEXA/ALPHA) não foi confirmada.
- Sem mipmaps (TEX1.MXL = 0 em todos os vistos).

### Conferência visual

Convertidas para `work/sample/` (não versionado): uma moldura de HUD 648×456 PSMT4 (branco + alfa; legível sobre fundo preto), um fundo 648×456 PSMT8 e uma folha de sprites de fogo 456×480 PSMT8 (5×6 quadros), um gradiente 424×96 PSMT8, um conjunto de 3 imagens. Todas coerentes (sem cisalhamento, cores contínuas, o que confirma largura linear e o unswizzle CSM1).

## 3. `pBAX` e `pth` (alto nível)

- **Correção**: as entradas com extensão `pth` **não** começam com `pBAX`. Elas começam com `u32` = tamanho da própria entrada, `u32 0x100`, `float 0.5`, `u32` contagem e depois registros de 0x20 bytes com floats (coordenadas XY na faixa de tela e quatro `128.0`, que parecem RGBA neutro do PS2). Hipótese: caminhos/polígonos 2D para UI ("path"). Não confirmado no código.
- **`pBAX`** (327 entradas, sem extensão) é um **banco de sons tipo VAB** (o VAB do PS1 usa `pBAV`): cabeçalho `pBAX`, `u32` = tamanho do corpo ADPCM, `u32 0`, `u16` nº de tons, `u16 0x10` (tamanho do cabeçalho); depois registros de 0x20 bytes com índice, parâmetros de nota/volume, um par ADSR com os valores padrão da SPU2 (ADSR1/ADSR2) e `u32` offset dentro do corpo. A **entrada seguinte** no fpk é o corpo: SPU ADPCM cru (quadros de 16 bytes, primeiro quadro zerado), cujo tamanho == campo +4 do `pBAX`. Campos de nota/pitch não decodificados.

## Incertezas

1. Significado exato dos dois u16 de nota/afinação no `pBAX` e do byte após o 1 nos seus registros.
2. Alfa: dobrar (0x80 -> 255) é a convenção PS2 usual; as paletas com alfa > 0x80 sugerem que alguns materiais usam outra escala/blend.
3. Listas DMA duplicadas no `wZIM` (por que duas) e o `u64` dos registros de paleta (assumido molde de TEX0).
4. Formato `pth` e os novos tipos dentro dos fpk descomprimidos (`csf`, `mpk`, `chf`, `cvf`, `hi4`, `HE*_`, `HPK_` …) não analisados.
5. Quem chama o decoder para IMAGE.BIN especificamente (só um chamador foi lido: `FUN_00252084`); não muda o algoritmo, que está validado pelo tamanho exato e pela estrutura das saídas (não há checksum para comparar byte a byte).
6. A interpretação blocos/páginas GS dos u16 em +0x08/+0x0A do registro de imagem foi conferida em 5 amostras (PSMT4 e PSMT8), não em todas.
