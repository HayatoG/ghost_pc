# Módulos IOP (IRX / IOPRP)

Fonte: `work/MODULES/` (extraído da ISO; não versionado). Análise gerada por `tools/iop_modules.py`
(`python -I ../tools/iop_modules.py work/MODULES --json logs/iop_modules.json`; log em `logs/iop_modules_<data>.log`).
Nenhum byte do jogo está neste documento; só nomes, versões e números de export/import.

Método: parsing do ELF IRX (tipo 0xFF80), seção `.iopmod` (0x70000080) para nome/versão do módulo, varredura de
tabelas de export (`0x41C00000`) e import (`0x41E00000`; stubs `jr ra; li $0,N` = número do import). O número de
exports é a quantidade de funções a partir do slot 4 (slots 0-3 são reservados: `_start`, reservado, shutdown, reservado)
e é aproximado (contado pelas relocações). Os binários não trazem nomes de função; os números abaixo são os
índices de export da biblioteca.

Versão do SDK: strings no ELF `PsIIlibcdvd 2800`, `PsIIlibsdr 2800`, `PsIIlibmtap 2800` e a imagem `IOPRP280.IMG`
indicam SDK Sony 2.8.0.

## 1. Módulos carregados pelo jogo (`work/MODULES/*.IRX`)

| Arquivo | Módulo (.iopmod) | Versão | Tam. | Exporta (lib ver, nº funções) |
|---|---|---|---|---|
| SIO2MAN.IRX | `sio2man` | 3.0 | 6641 | sio2man 2.4 (~60) |
| MTAPMAN.IRX | `multitap_manager` | 3.2 | 11029 | mtapman 2.4 (3) |
| PADMAN.IRX | `padman` | 4.5 | 43813 | padman 2.3 (14) |
| CDVDSTM.IRX | `cdvd_st_driver` | 2.2 | 34705 | cdvdstm 1.1 (1) |
| MCMAN.IRX | `mcman` | 2.34 | 95909 | mcman 2.7 (58) |
| MCSERV.IRX | `mcserv` | 2.16 | 7417 | mcserv 2.1 (4) |
| LIBSD.IRX | `Sound_Device_Library` | 3.3 | 28605 | libsd 1.5 (30) |
| SDRDRV.IRX | `sdr_driver` | 4.1 | 9161 | sdrdrv 1.1 (2) |
| EZSOUND.IRX | (nome vazio; string interna "cavia EzSound driver version 2.0") | 0.0 no header / 2.0 na string | 12041 | nenhum (módulo só-RPC) |

Observações: `padman 4.5`, `mtapman 3.2` e `mcserv/mcman` conferem com as strings de verificação de versão no ELF
(`[libpad.a = %d.%d, padman.irx = %d.%d]`, `[libmtap.a = ..., mtapman.irx = ...]`, `libmc: too old release of mcserv.irx/mcman.irx`).
EZSOUND é middleware próprio de terceiros (string "cavia", desenvolvedora), não do SDK Sony.

### Imports (biblioteca versão: números de import)

| Módulo | Imports |
|---|---|
| SIO2MAN | loadcore 6; intrman 4,5,6,7,17,18; stdio 4; dmacman 28,32,33,34,35; thbase 4,6,20; thevent 4,6,7,8,10; thsemap 4,6,8,11; sysclib 8,29,36 |
| MTAPMAN | loadcore 6; stdio 4; thbase 4,6,8,14,20; thevent 4,6,10; sifman 5,29; sysclib 8,29,36; sifcmd 14,17,19,22; **sio2man 48,51,52,53,54,55,56** |
| PADMAN | sysmem 14; loadcore 6; intrman 17,18; stdio 4; **sio2man 11,46,51,52,57,58,60**; thbase 4,5,6,8,20,22,23; thevent 4,5,6,7,8,10; sifman 5,7,8,29; sifcmd 14,17,19,22; vblank 8,9; sysclib 8,29,36 |
| CDVDSTM | sysmem 14; loadcore 6,7; intrman 17,18,23; stdio 4; thbase 33,36,37,38; thevent 6-10; thsemap 4,5,6,8; sifman 7,8; ioman 20,21; **cdvdman 11,12,39,40,46,48,49,50,61,114** |
| MCMAN | loadcore 6,7; intrman 17,18; **sio2man 24,25,26,57,59**; sysclib 12,14,20,22,23,27,29,30; stdio 4; thbase 33,35,39; thsemap 4-8; modload 13; ioman 20,21; **secrman 4,5,6**; cdvdman 24 |
| MCSERV | loadcore 6,7; intrman 17,18; sysclib 12,14,29,36; stdio 4; sifman 5,7,8,29; sifcmd 14,17,19,22,23,24,25; thbase 4,5,6,10,14,20,22,33; thsemap 6,8; **mcman 2.7: 6-16,21-24,26,36-39,42,45,53** |
| LIBSD | intrman 4,5,6,7,17,18,23; loadcore 5,6; sifman 7,8; sysclib 17; thevent 4-10 |
| SDRDRV | **libsd 1.5: 4-20,23-27,30-33**; sysmem 14; intrman 17,18; loadcore 6,7; sifcmd 14,15,16,17,19,22,24,25; sysclib 8,29,36; thbase 4,5,6,10,14,20,22,24,26,28 |
| EZSOUND | **libsd 1.5: 4,5,6,7,9,10,11,16,17,19,23,25**; intrman 17,18; loadcore 5; sifcmd 14,17,19,22; stdio 4; sysclib 12,14; thbase 4,6,20; thsemap 4,6,8; vblank 5 |

Dependências relevantes (ordem de carga necessária): `sio2man` -> `padman`, `mtapman`, `mcman`; `secrman` (ROM, não
está em MODULES/) -> `mcman`; `cdvdman` (do IOPRP) -> `cdvdstm`, `mcman`; `mcman` -> `mcserv`; `libsd` -> `sdrdrv`, `ezsound`.
`secrman`, `vblank`, `intrman` e `dmacman` não estão em `MODULES/` nem no IOPRP280 (seção 3); vêm do ROM do console (rom0:).

## 2. Uso no jogo e ligação com `SLES_530.20`

Strings do ELF (offsets de arquivo; ELF PS2 carrega com base próxima de 0x100000, offsets aqui são do arquivo extraído):

| Offset | String | Significado |
|---|---|---|
| 0x36afd8 | `cdrom0:\MODULES\IOPRP280.IMG;1` | imagem usada no reboot do IOP (`sceSifRebootIop`/UDNL) |
| 0x37a8f0 | `rom0:UDNL ` | prefixo do comando de reboot do IOP (argumento `rom0:UDNL cdrom0:\MODULES\IOPRP280.IMG;1`) |
| 0x36aff8 | `\MODULES` | diretório de módulos |
| 0x3751b8..0x375238 | `sio2man.irx`, `mtapman.irx`, `padman.irx`, `cdvdstm.irx`, `mcman.irx`, `mcserv.irx`, `libsd.irx`, `sdrdrv.irx`, `ezsound.irx` | lista de módulos carregada pela tarefa `CStacomTaskSetup` (RTTI `16CStacomTaskSetup` em 0x3751a0), nesta ordem |
| 0x368b40 / 0x368b90 | `loadModules [2]` / `loadModules complete.` | log do carregamento |
| 0x368e68 | `ezSound_RPCInit: ok.` | cliente do EZSOUND (RPC) |
| 0x311680 / 0x313240 / 0x3136e8 | `PsIIlibcdvd 2800` / `libsdr` / `libmtap` | versões das libs EE linkadas |
| 0x378820..0x378b08 | `sceCdCbfunc`, `Libcdvd bind err ...`, `sceCdStRead/Pause/Resume/Stat` | uso de libcdvd, incluindo **streaming** (CDVDSTM) |
| 0x378b78 | `sceSdRemoteInit() RPC bind error!` | libsdr (SDRDRV) |
| 0x378be0 / 0x378c08 / 0x378c30 | `libmc: too old release of mcserv.irx/mcman.irx`, `sceMcUdCheckNewCard RPC faild` | libmc (MCSERV/MCMAN) |
| 0x378038 | `[libpad.a = %d.%d, padman.irx = %d.%d]` | libpad (PADMAN) |
| 0x379517..0x379618 | `libmtap: ...`, `sceMtapPortOpen/Close/GetConnection/...` | libmtap (MTAPMAN) |
| 0x381010 / 0x381388 / 0x3813a0 | `CDVD`, `cdrom0:`, `host0:` | dispositivos de IO usados; `host0:` e `./filelist.bin`, `CDVDFromPC` (0x368cd8) indicam caminho de desenvolvimento |

Nota: a ordem/linha de carga é inferida da tabela de strings contígua e do log `loadModules`; as chamadas exatas
(`sceSifLoadModule` com o prefixo `cdrom0:\MODULES\`) ainda não foram confirmadas por xref no ReVa.

| Módulo | Serviço | Uso no jogo |
|---|---|---|
| SIO2MAN | Driver SIO2 (porta serial dos controles e memory cards) | base para pad, multitap e memory card |
| MTAPMAN | Multitap | libmtap presente (strings `sceMtapPort*`); jogo provavelmente só consulta/abre portas |
| PADMAN | Controles DualShock | libpad (`padman.irx 4.5`), input do jogador |
| CDVDSTM | Streaming de CD/DVD (`sceCdSt*`) | leitura em streaming (provavelmente BGM.BIN / vídeos MOV*.BIN / VAG streams) |
| MCMAN / MCSERV | Memory card (mcman = driver; mcserv = RPC para EE / libmc) | save/load (mensagens de autosave nas strings) |
| LIBSD | Sound Device library (SPU2), baixo nível no IOP | base de SDRDRV e EZSOUND |
| SDRDRV | Servidor RPC `sceSdRemote` (libsdr) | EE controla o SPU2 via `sceSdRem*` |
| EZSOUND | Middleware de som do jogo (cavia), usa libsd direto | `ezSound_RPCInit`, `CVagStreamControl` (streaming VAG) |

## 3. IOPRP280.IMG (imagem de reset do IOP)

Formato: ROMDIR (entradas de 16 bytes: nome[10], extinfo u16, tamanho u32; dados alinhados a 16 bytes, na ordem
RESET -> ROMDIR -> EXTINFO -> módulos). Tamanho total 269 889 bytes; tabela de 20 slots (RESET, ROMDIR, EXTINFO + 16 módulos + terminador). Entrada = nome curto de 8 chars; módulo = nome interno do `.iopmod`.

| # | Entrada | Módulo interno | Versão | Tam. | Exporta |
|---|---|---|---|---|---|
| 1 | SYSMEM | System_Memory_Manager | 2.1 | 5305 | sysmem |
| 2 | LOADCORE | Module_Manager | 2.5 | 10433 | loadcore |
| 3 | SIFCMD | IOP_SIF_rpc_interface | 2.8 | 10529 | sifcmd |
| 4 | SIFMAN | IOP_SIF_manager | 2.3 | 5969 | sifman |
| 5 | THREADMAN | Multi_Thread_Manager | 2.3 | 39405 | thbase, thevent, thsemap, thmsgbx, thfpool, thvpool, thrdman |
| 6 | IOMAN | IO/File_Manager | 2.3 | 12545 | ioman |
| 7 | MODLOAD | Moldule_File_loader | 2.8 | 18533 | modload |
| 8 | FILEIO | FILEIO_service | 2.16 | 17909 | (serviço RPC `fio`) |
| 9 | CDVDMAN | cdvd_driver | 2.35 | 80773 | cdvdman |
| 10 | CDVDFSV | cdvd_ee_driver | 2.35 | 31981 | cdvdfsv |
| 11 | LOADFILE | LoadModuleByEE | 2.2 | 10505 | (serviço RPC `sceSifLoadModule`) |
| 12 | TIMEMANI | Timer_Manager | 2.2 | 6085 | timrman |
| 13 | ROMDRV | ROM_file_driver | 2.1 | 3881 | romdrv |
| 14 | EESYNC | SyncEE | 2.1 | 1545 | eesync |
| 15 | SYSCLIB | System_C_lib | 2.3 | 10045 | sysclib, stdio |
| 16 | STDIO | Stdio | 2.3 | 3377 | stdio |

Note que o IOPRP não contém `intrman`, `dmacman`, `vblank`, `heaplib` nem `secrman`; esses vêm do ROM do console (rom0:). Os módulos acima
correspondem ao conjunto padrão do SDK 2.8 (DVD, com `cdvdman 2.35` e `cdvdfsv`).

## 4. Relevância para o port

O port não executa o IOP; emula o **contrato EE <-> IOP** (RPCs via SIF) no nível das bibliotecas EE. Serviços a prover:

1. **Pad (libpad / PADMAN + SIO2MAN)**: `scePadInit/PortOpen/Read/SetMainMode/SetActDirect/...`. Mapear para SDL/XInput/GLFW:
   gamepad com sticks analógicos, botões com pressão (DualShock 2), rumble opcional. Estado `scePadGetState` deve reportar
   "stable"/"findpad" e o modo DualShock. Dificuldade baixa.
2. **Multitap (libmtap / MTAPMAN)**: `sceMtapPortOpen/GetConnection/...`. Pode ser stub: reportar multitap não conectado. Baixa.
3. **Memory card (libmc / MCMAN + MCSERV)**: `sceMcInit/Open/Read/Write/Close/GetDir/Format/CheckNewCard/GetInfo/...`.
   Emular com diretório host (`saves/`) ou imagem `.ps2` de 8 MB; manter o contrato assíncrono (`sceMcSync`), estrutura de
   diretório e checagem de tipo/formatação do cartão. Requer cuidado com as strings de autosave. Média.
4. **Som (libsdr / SDRDRV + LIBSD + EZSOUND)**: o EE chama `sceSdRem*` (RPC) para controlar o SPU2 (vozes, volumes,
   reverb, transferências para a SPU RAM via `sceSdVoiceTrans`/`sceSdBlockTrans`, callbacks). EZSOUND é um segundo serviço
   RPC próprio que tem de ser reimplementado a partir da decompilação do módulo (não é do SDK público): comandos
   RPC (`ezSound_*`), streaming de VAG (`CVagStreamControl`). É a parte mais trabalhosa: precisa de emulação de SPU2
   (ADPCM VAG, ADSR, reverb) + mixer. Alta.
5. **CD/DVD (libcdvd / CDVDMAN + CDVDFSV + CDVDSTM)**: `sceCdInit/Read/Seek/GetError/DiskReady/SearchFile/Sync`, streaming
   `sceCdStInit/Start/Read/Pause/Resume/Stat` e `sceCdReadClock`. Mapear para leitura de arquivos do dump na máquina
   host (a imagem ISO ou diretório extraído), com tabela de LBA -> arquivo; streaming com buffer em anel simulando o
   buffer do IOP. Crítico para carregar todos os assets. Média.
6. **Boot de IOP / loader (LOADFILE, MODLOAD, UDNL, IOPRP)**: `sceSifRebootIop`, `sceSifLoadModule`, `sceSifInitRpc`, `sceSifBindRpc`
   viram operações de "sucesso imediato" no port; nada do IOPRP precisa ser executado, só a lista de módulos serve como
   documentação do que o jogo espera encontrar.
7. **FILEIO / IOMAN (`fioOpen/Read`, `sceOpen`...)**: usados para `host0:` e `cdrom0:`; redirecionar para o sistema de arquivos host.
8. **Timer/IOP sincronização (TIMEMANI, EESYNC, SIFCMD)**: sem equivalente necessário no port.

### Próximos passos sugeridos

- Importar os IRX no ReVa (`/MODULES`, `MIPS:LE:32:default`) para decompilar `EZSOUND.IRX` (único módulo sem documentação pública).
- Fazer xref no ELF das strings `sio2man.irx`..`ezsound.irx` (0x3751b8..) e do `rom0:UDNL` (0x37a8f0) para confirmar a ordem e os argumentos de `sceSifLoadModule`/`sceSifRebootIop`.
- Mapear os IDs de RPC (`sceSifBindRpc`) usados por cada lib no ELF e registrar em `docs/`.
