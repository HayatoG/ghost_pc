# Boot do IOP e RPCs (EE -> IOP)

Fonte: xref/decompilação no ReVa (`/SLES_530.20`). Endereços são vaddr do ELF (os offsets de arquivo em
`iop-modules.md` §2 valem vaddr = offset + 0x100000, ex.: `sio2man.irx` 0x3751b8 -> 0x4741b8).
Nenhum byte do jogo aqui, só nomes, IDs e endereços. Confiança: high / medium / low.

## 1. Sequência de boot

`main` (0x1943e0) -> `GameBootInit` (0x265794, nome palpite, low) -> primeira ação:
`IopBootLoadModules(*DAT_004027b0, 0x403380, 9)` (0x1ca574; medium: nome palpite, comportamento confirmado).
`param_1` = flag de modo host (`!=0` = desenvolvimento, sem reboot). Modo de disco (flag == 0), a que o port deve cobrir:

1. `sceSifInitRpc(0)` (0x2d1588) — high.
2. Loop `sceSifRebootIop(0x469fd8)` até retornar != 0 (0x1ca5b0), arg = `cdrom0:\MODULES\IOPRP280.IMG;1`
   (string 0x469fd8, única xref aqui). `sceSifRebootIop` (0x2d71b0) monta `"rom0:UDNL "` + arg numa pilha de 80 bytes
   (erro "too long parameter" se > 80) e chama o reset (FUN_002d6fe0) — high. A string `rom0:UDNL ` é literal dentro da função.
3. Loop `sceSifSyncIop()` (0x2d7160; `sceSifGetReg(4)&0x40000`) até completar — high.
4. `sceSifInitRpc(0)`; reset dos flags de bind de LOADFILE (0x2d6410) e FILEIO (0x2d2f40) — medium.
5. `sceCdMmode(2)` (0x2ae9d0; S-cmd 0x22, mídia 2 = DVD) em loop até != 0 (0x1ca554) — high.
6. Monta contexto de caminho (0x1c8760) com: prefixo `cdrom0:`, separador `\`, sufixo `;1`, diretório `\MODULES` (0x469ff8).
   Para cada entrada da tabela: nome é convertido para MAIÚSCULAS e `IopBuildModulePath` (0x1c8c10) gera
   `cdrom0:\MODULES\<NOME>.IRX;1`; chama `sceSifLoadModule(path, args_len, args)` (0x2d6c80) até 10 tentativas
   enquanto o retorno for < 0; o retorno (module id) é gravado em `entry[2]` se não nulo. — high (decompilação direta).
7. Tabela em 0x403380: 9 entradas de 12 bytes `{char *name, char *args, int *result}`; **todos `args`/`result` = NULL**, logo
   `sceSifLoadModule(path, 0, NULL)` (sem argumentos de módulo; bytes enviados = 0). Ordem real de carga (names em 0x4741b8, passo 0x10):

| # | Nome | Caminho completo montado |
|---|---|---|
| 1 | sio2man.irx | `cdrom0:\MODULES\SIO2MAN.IRX;1` |
| 2 | mtapman.irx | `cdrom0:\MODULES\MTAPMAN.IRX;1` |
| 3 | padman.irx | `cdrom0:\MODULES\PADMAN.IRX;1` |
| 4 | cdvdstm.irx | `cdrom0:\MODULES\CDVDSTM.IRX;1` |
| 5 | mcman.irx | `cdrom0:\MODULES\MCMAN.IRX;1` |
| 6 | mcserv.irx | `cdrom0:\MODULES\MCSERV.IRX;1` |
| 7 | libsd.irx | `cdrom0:\MODULES\LIBSD.IRX;1` |
| 8 | sdrdrv.irx | `cdrom0:\MODULES\SDRDRV.IRX;1` |
| 9 | ezsound.irx | `cdrom0:\MODULES\EZSOUND.IRX;1` |

Modo host (flag != 0): sem reboot; `sceCdInit(0)` em loop, caminho com prefixo `host0:`, separador `/`, busca
`modules:~/sce/iop/modules:/usr/local/sce/iop/modules` (0x46a008); mesmo loop de LoadModule (uma tentativa). É caminho de dev; o port ignora.

Notas: `sceSifLoadModule` usa o serviço LOADFILE (bind 0x80000006, `_sceLoadFileBind` 0x2d6280, init func 0xff; high),
buffer de requisição 0x5d8640 (path[0xfc] + args até 0xfc, 0x200 de envio). Strings `loadModules [2]`, `loadModules complete.`,
`sceSifInitRpc complete.`, `ezSound_RPCInit: ok.` (0x467b40..0x467e68) **não têm xref** (printf removido/não ligado no build): não
use-as como evidência de ordem. As chamadas ficam só a partir daí no boot; ezSound é inicializado depois (§3).

O port deve emular: `sceSifRebootIop` -> sucesso (1) imediato; `sceSifSyncIop` -> 1; `sceCdMmode` -> 1; `sceSifLoadModule` ->
id positivo crescente (ordem acima; ids só são guardados em `entry[2]`, nunca usados depois, medium).
Qualquer `sceSifBindRpc` deve devolver cliente com `server != NULL`.

## 2. IDs de RPC (sceSifBindRpc) encontrados

Todos os 19 call sites de `sceSifBindRpc` (0x2d1d20); IDs 32-bit (sign-extended no decompilador).

| ID | Serviço | Chamador (endereço) | Buffer cliente | Conf. |
|---|---|---|---|---|
| 0x80000001 | FILEIO (fio) | `fioInit` 0x2d2ca8 (lazy, chamado por 16 wrappers fio*) | 0x5d8380; também registra cmd 0x80000011/13 | high |
| 0x80000003 | IOP heap | `sceSifInitIopHeap` 0x2d5d30 | 0x5d8480 | high |
| 0x80000006 | LOADFILE | `_sceLoadFileBind` 0x2d6280 | 0x5d8840 | high |
| 0x80000100 / 0x80000101 | PADMAN (2 servidores) | `scePadInit` 0x2a1a78 | 0x5d1240 / 0x5d1268 | high |
| 0x80000400 | MCSERV | `sceMcInit` 0x2af8e8 | 0x5d2b40 | high |
| 0x80000592 | CDVD init | `sceCdInit` 0x2adf90 | 0x5d1928 | high |
| 0x80000593 | CDVD S-cmd | `_sceCdSCmd` 0x2add70 | 0x412200 | high |
| 0x80000595 | CDVD N-cmd | `_sceCdNCmd` 0x2ada58 | 0x411850 | high |
| 0x80000597 | CDVD SearchFile | `sceCdSearchFile` 0x2ad720 | 0x5d1900 | high |
| 0x8000059a / 0x8000059c | CDVD DiskReady (old / new) | 0x2ae270 / `sceCdDiskReady` 0x2ae468 | 0x5d1950 | high |
| 0x80000701 | SDRDRV (sceSdRemote) | `sceSdRemoteInit` 0x2af078 | 0x5d1a40 | high |
| 0x80000901..905 | MTAPMAN (5 servidores) | `sceMtapInit` 0x2bc4a0 | 0x5d4180, 41a8, 41d0, 41f8, 4220 | high |
| **0x04649765** | **EZSOUND** (ID proprietário Cavia) | `ezSound_RPCBind` 0x1959c4 | 0x4c9a90 | high |

Não observado: bind do streaming CDVD (`sceCdSt*`, normalmente 0x80000596) — não aparece como `sceSifBindRpc` direto (possível
chamada via ponteiro/outra rotina); marcar para investigar. SDR: só o bind 0x80000701; as chamadas `sceSdRem*` usam o mesmo cliente 0x5d1a40.
Pad/mc/sdr/cdvd seguem as libs públicas do SDK 2.8: o port deve implementar os comandos pelo nome da biblioteca, não por ID.

## 3. EZSOUND (cliente no ELF)

`ezSound_RPCBind` (0x1959c4, high): `sceSifInitRpc(0)`; bind `0x04649765` no cliente 0x4c9a90 até `server != 0`; retorna -1 se o bind falhar.
Chamado por `ezSound_Init` (0x19a870, nome palpite) logo depois de `sceSifInitIopHeap`; `ezSound_Init` zera buffers 0x56bf80 (0x2000),
0x56df80 (0x2000), 0x56ff80 (0x200), 0x570180 (0x200), aloca 0x28000 bytes no IOP (`sceSifAllocIopHeap` FUN_002d5db8) e dispara
RPC func 1 sem dados (inicialização/reset).

API real do módulo (a partir do cliente; apenas 2 números de função usados):

| func | Chamada | Envio | Recepção |
|---|---|---|---|
| 0 | `ezSound_RPCCall(0, 0x56df80, 0x2000, 0x570180, 0x200)` em `ezSound_FrameUpdate` 0x19b184 | lista de comandos (cabeçalho u32 = nº de registros de 8 bytes, depois registros) | 0x200 bytes: estado por voz (8 slots de 12 bytes copiados para 0x56ff80, flags de "voz ainda ativa") |
| 1 | `ezSound_RPCCall(1,0,0,0,0)` em `ezSound_Init` | nenhum | nenhum |

`sceSifCallRpc` é chamado com modo `recv_size != 0` e antes `FlushCache(0)`; `ezSound_RPCIsIdle` (0x195ac8) faz `sceSifCheckStatRpc==0`
para saber se a chamada anterior terminou (se não, o frame não envia novo lote).
Comandos (FUN_00199d1c, palpite low): registros de 8 bytes `{u16 n; u16 voice | (op << 8); u32 value}`; ops vistas: `0x2040|voice` (n=3, valor = ponteiro/endereço de dados na SPU RAM?),
`0x300|voice` (n=1, valor u16, possivelmente volume), `0x400|voice` (n=1, possivelmente outra volume/pitch). Máquina de estados por voz
(estados 1..7 em +0x10) fecha em máscaras de voz (`DAT_005ddd90`, `+0x98`, `+0xa0`: key-on/off/pitch mask). Decodificar o
formato exato exige decompilar `EZSOUND.IRX` (não feito aqui).

O port deve emular: bind EZSOUND OK; func 1 = reset; func 0 = consumir buffer de comandos de 0x2000 B e devolver 0x200 B de status
(zerado = nenhuma voz ativa serve de stub silencioso seguro; se o jogo esperar fim de voz, devolver os 8 slots com flag zero).
Para som real: interpretar comandos de voz (key on/off, volume, pitch, endereço SPU2) e mapear para um mixer próprio; carregamento de VAG/dados SPU
é por `sceSdRem*` (SDRDRV) e streaming (`CVagStreamControl`).

## 4. Incertezas

- Nomes `GameBootInit`, `ezSound_Init`, `ezSound_FrameUpdate`, `ezSound_EncodeVoiceCmd`, `IopPathCtxInit` são palpites (marcados `[guess]`).
- Layout dos registros EZSOUND e significado dos ops: low.
- `*DAT_004027b0` (flag host) não rastreado até a origem; em disco real deve ser 0.
- Os módulos estão carregados após um único reboot; não foi vista nenhuma segunda chamada de `sceSifRebootIop` (só 1 xref).
