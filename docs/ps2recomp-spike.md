# Spike PS2Recomp (ran-j/PS2Recomp) sobre SLES_530.20

Status em 2026-11-03: **`ps2_recomp.exe` compilado** (`third_party/PS2Recomp/out/build/ps2xRecomp/`, BUILD_OK 123/123) e **ainda não executado** (usuário pausou antes). Exportação pronta.

## Entradas prontas
- Código: `third_party/PS2Recomp` (clone raso do commit `e5636981`, 2026-10-23; GPL-3.0; **fora do git**, `/third_party/` no `.gitignore`).
- Projeto Ghidra headless e fixo: `ghost_pc/work/ghidra_project` (projeto `spike`, programa `/SLES_530.20`, linguagem `r5900:LE:32:default`), com 166 nomes do SDK (`docs/symbols.csv`) + 20 do boot do IOP (`docs/symbols_iop.csv`), aplicados por `tools/ghidra/ApplySymbolsCsv.java`.
- Export para o recompilador: `ghost_pc/work/recomp/ghost.toml` + `ghost_functions.csv` (7462 funções, 31 583 labels executáveis, 140 stubs por endereço), gerados por `ExportPS2Functions.java` do próprio PS2Recomp em modo headless. Regerar sempre que os nomes mudarem e, depois, corrigir o `input` do TOML para `ghost_pc/work/SLES_530.20` (o Ghidra grava o caminho de importação antigo).
- Comando headless (ver `docs/tools.md` na raiz): argumentos do `askFile` vão como **argumentos do script** (`-postScript ExportPS2Functions.java <toml> <csv>`); o `.properties` não funcionou.

## Toolchain instalado (winget, 2026-11-03)
- Visual Studio Build Tools 2022 17.14.44 (MSVC 14.44.35207, workload VCTools), CMake 4.4.4, Ninja — CMake/Ninja ficam em `%LOCALAPPDATA%\Microsoft\WinGet\Packages\...` (fora do PATH de sessões já abertas).
- Build usado: `vcvars64.bat` → `cmake -S . -B out\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF` → `cmake --build out\build`. Só o `ps2xRecomp` foi construído; runtime, analisador, IOP e testes seguem desligados. Rodar via `cmd /c` com um `.bat` (o `vcvars64` precisa do ambiente de cmd).

## Resultado da primeira execução (2026-11-03)
`ps2_recomp ghost.toml` (com `low_memory_mode = true`, `output_worker_threads = 2`): **exit 0, 67 s**, "Recompilation completed successfully". Log: `logs/ps2_recomp.log`.
- 39 045 funções extraídas do ELF (7462 do Ghidra + entradas internas descobertas pelo recompilador), 70 seções, 214 188 pontos de retomada em 6550 funções donas.
- Saída em `work/recomp/output/`: 39 046 `.cpp` + 2 `.h`, **248 MB**. Maiores: `register_functions.cpp` (19 MB), `ps2_recompiled_functions.h` (3,4 MB), funções individuais até 2,9 MB.
- 4520 avisos, **todos** do tipo "unresolved JR/JALR … promoted N fallback entries" (saltos indiretos/jump tables não resolvidos, tratados com entradas de fallback). Nenhum aviso de instrução MMI/VU0 não suportada, nenhum erro.
- Limitação: ainda **não compilamos o C++ gerado** nem rodamos nada; o recompilador só prova que a tradução estática passa. Falta runtime (GS/VU1/SPU2/IOP).

## Build do runtime com o C++ gerado (2026-11-03)
- Árvore de build: `work/ps2recomp_game/` (cópia do PS2Recomp + C++ gerado em `ps2xRuntime/src/runner` e `include`); nossas mudanças em `patches/` (ver `patches/README.md`).
- **Build 1** (`out/build`; logs agressivos ON, `/GL`+`/LTCG` ON): 1331/1332 unidades OK e o **link falhou com `Stack overflow`** na geração de código do LTCG, após ~1 h de CPU e ~9 GB de RAM. O `/GL` empurra a geração de código para o link; com 248 MB de C++ e funções enormes o otimizador estoura a pilha.
- Descoberta: `PS2X_ENABLE_AGRESSIVE_LOGS=ON` (padrão) define `PS2_FUNCTION_LOG_TRACKER` e insere `PS_LOG_ENTRY` em cada função gerada (log com flush) — inviável para medir FPS.
- **Build 2** (`out/build_perf`): `-DPS2X_ENABLE_LTCG=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF`, 10 jobs. Resultado: `[PREENCHER]`.
- Raiz do `cdrom0:` do runtime = diretório do ELF (`work/`); por isso `IMAGE.BIN`, `BGM.BIN`, `RTDEN2EU.BIN` e `MOV00EUS.BIN` foram extraídos para lá (vídeos `MOV00/01/02EU.BIN`, ~2,2 GB, ainda não).
- Controle e medição (patch): `PS2X_TARGET_FPS`, `PS2X_PAD_SCRIPT`, `PS2X_FPS_LOG`; launcher: `run --fps N --pad-script ARQ --fps-log CSV`.

### Primeira execução (build 2, 2026-11-03)
- **Build 2 OK** (`out/build_perf`, sem LTCG/logs): ~14 min do zero, `ps2EntryRunner.exe` 130 MB; incremental ~minutos.
- Teste do pad virtual (`vpad_test`) passou (build via CMake: `-DPS2X_BUILD_VPAD_TEST=ON`). Um `.exe` compilado direto com `cl` foi barrado pelo **Smart App Control** (ver abaixo).
- Run de fumaça (`pad_scripts/smoke.txt`, 31 s): host 59,8 FPS, p99 17,25 ms, `guest_vsync_hz`=49,99 (PAL ok). **Quadros magenta puro** (nada desenhado) e `guest-branch:missing-target kind=IndirectCall op=JALR source=0x1931a0 target=0x3067c8`.
- Causa: `0x3067c8` é uma função vazia (`jr ra; nop`) sem chamadores diretos; o Ghidra não a criou e o recompilador não gerou o alvo. É a mesma classe dos 4520 saltos indiretos não resolvidos.
- Correção: `tools/ghidra/CreateMissingFunctions.java` cria funções nos vãos do `.text` não cobertos (2140 vãos / 114 KB; +1428 funções → **8890** no total). TOML regerado e pós-processado por `tools/fix_recomp_toml.py` (o exportador reescreve o `input` e apaga `low_memory_mode`). **Pendente:** rodar o `ps2_recomp`, copiar o C++, rebuild e nova captura.
- **Smart App Control** (estado 1 no registro): bloqueia executáveis sem assinatura/reputação (Code Integrity ids 3077/3033). Não respeita exclusões do Defender. O usuário o desligou (estado 0) em 2026-11-03.
- Magenta = cor de "sem frame" do runtime; é o primeiro indicador visual de que o jogo ainda não desenhou.

### Segunda rodada (2026-11-03): 41.714 funções, boot do IOP e laço no fioInit
- Recompilador com as 8890 funções do Ghidra: **41.714 funções** no C++ (252 MB), 4541 avisos (só `JR/JALR`). Rebuild ~14 min do zero; relink ~2 min.
- **Estouro de pilha** (`0xC00000FD`, 4 s após abrir): pilha padrão de 1 MB insuficiente. Corrigido com `/STACK:256 MB` (opção `PS2X_STACK_RESERVE_MB`) — só relink.
- Depois disso: sem quedas, **zero alvos ausentes** (`PS2X_MISSING_LOG`), 59,9 FPS de host, `realtime_pct` 100%, mas **quadro magenta** (nada desenhado).
- **IOP**: o runtime carrega os 9 IRX reais na ordem esperada (sio2man, mtapman, padman, cdvdstm, mcman, mcserv, libsd, sdrdrv, ezsound), com as mensagens originais dos módulos.
- **Batimento** (`PS2X_HEARTBEAT`): `pc=0x2d2d38 ra=0x2d2d6c` constante por 60 s; `dma=0 gif=0 gsw=0`; 1 thread. O PC está em `fioInit` (`0x2d2ca8`), em loop de `sceSifBindRpc(…, 0x80000001)`.
- **Causa**: o `ps2xIOP` só tem serviços HLE para mcserv, dbcman e libsd; o runtime emula a API de arquivos **por nome** (`fioOpen`, `sceOpen`… no `ps2_call_list.h`), sem RPC. O `sceSifRebootIop` do runtime só reseta o IOP e ignora a imagem `IOPRP280.IMG`. As funções fio do jogo não têm o nome do SDK no Ghidra, então foram recompiladas como código real.
- **Plano**: mapear sistematicamente os ~700 nomes do `ps2_call_list.h` às funções do jogo (agentes: `symbols_fio.csv`, `symbols_sdk2.csv`), reaplicar com `tools/ghidra/ApplySymbolsCsv.java`, reexportar e repetir o ciclo.

### Terceira rodada (2026-11-03 03:00–03:40): latch do DMAC, display configurado, espera do CD
- **Ciclo 1** (nomes `fio`, 29 funções; `ret0` em `fioIoctl`/`fioInit`): o jogo passou do `fioInit`, cria a 2ª thread e começa a fazer DMA; parou em `WaitSema` (`0x2ceda8`).
- **Diagnóstico do semáforo** (decompilação headless com `tools/ghidra/DecompileAt.java`): o jogo espera um semáforo sinalizado por `FUN_0019caec`, chamada do **handler de DMA** `FUN_0019ca90` (registrado com `AddDmacHandler` para as causas 1=VIF1 e 2=GIF). O handler limpa um bit e, ao zerar a máscara, envia o próximo pacote ou faz `iSignalSema`.
- **Pesquisa** (`research-dma-irq.md`): o PR #151 do PS2Recomp descreve o mesmo travamento (a libdma faz `sceDmaSend`, **depois** registra o handler, depois `WaitSema`; a causa levantada antes do handler se perde). **Análise Opus** (`dma-irq-analysis.md`): trace do caminho de DMA no runtime e hipóteses H1–H4.
- **Correção no runtime** (patch): (1) **latch** da causa DMAC quando mascarada ou sem handler utilizável, reentregue em `AddDmacHandler`/`EnableDmac`/`EnableDmacHandler`; (2) drenagem das causas concluídas **a cada VBlank**; (3) contadores `[hb-irq]` no heartbeat. Resultado medido: `latched=4 redelivered=1 queued=22 dma=25`, `pending_mask=0`, `not_recompiled=0`.
- **Efeito visível:** `dispfb1=0x9400`, `display1≠0` (o jogo configurou o display) e o quadro deixou de ser magenta: **preto de 640×512** (framebuffer do jogo ainda limpo). `guest_vsync_hz` caiu para **38 Hz (76% do tempo real)**: o jogo agora executa de verdade e o código recompilado é mais lento que o relógio PAL — essa é a métrica real a otimizar.
- **Nova espera:** `pc=0x2ade38` em `_sceCdSCmd` (`0x2add70`): laço de `sceSifBindRpc(…, 0x80000593)` esperando o servidor de comandos S do CD (inexistente no runtime). `sceCdMmode` e companhia são HLE por nome, mas esse wrapper interno é recompilado. Correção: ligar `_sceCdSCmd`/`_sceCdNCmd` a `ret1@ENDEREÇO` no TOML (ciclo 3).
- **Ferramenta**: `sync_recomp_output.py` só copia arquivos que mudaram, para o ninja ser incremental de verdade (antes recopiar tudo forçava ~14 min de rebuild por ciclo). O unity build agrupa por nome de arquivo; renomear funções desloca lotes, mas stubs por endereço mantêm o nome.

### Quarta rodada (03:40–04:30): primeira tela, CD, pad e PERFIL
- **Ciclo 3**: `ret1` em `_sceCdSCmd`/`_sceCdNCmd` (espera de `sceSifBindRpc(0x80000593)`) → o jogo passou. Mudar o TOML (stubs) altera `ps2_recompiled_stubs.h`, incluído por todo arquivo gerado: rebuild completo (~14 min). **Remendos por endereço agora vão em `src/lib/ghost_overrides.cpp`** (patch), 1 arquivo por mudança; o módulo precisa estar no *executável* (numa biblioteca estática o linker descarta o objeto sem símbolos referenciados).
- **CD**: `sceCdSearchFile` falhava para `MOV00/01/02EU.BIN` (não extraídos) e o jogo relia o LBN 0 em laço (1702 avisos). Extraídos para `work/` (o runtime usa o diretório do ELF como raiz do `cdrom0:`). Alternativa futura: configurar `IoPaths::cdImage` (hoje só pela UI) para ler direto da ISO.
- **PRIMEIRA TELA REAL**: menu de **seleção de idioma** (EN/FR/DE/ES/IT), 640×512, ~10 s após abrir.
- **Pad virtual**: chega ao jogo (`[hb-pad]` mostra `last_buttons=0xbfff` com CROSS e `used_override=1`; a máquina de estados do libpad do jogo terminou: estado `+0x108`=0, GetState=6, modo `0x41`). Mesmo assim o menu **não reage** (MD5 das capturas idêntico após DOWN/CROSS/START) — causa ainda desconhecida (o jogo pode esperar outro evento; investigar quais tarefas rodam no laço `main`).
- **Ferramentas novas**: `PS2X_PEEK` (lê palavras da RAM do guest no heartbeat), `PS2X_HEARTBEAT` com `[hb-pad]`/`[hb-irq]`, `tools/ghidra/DecompileAt.java` (decompilação headless em lote), amostrador `PS2X_PROFILE_*` + `tools/resolve_profile.py` + `/MAP` do linker.
- **DESEMPENHO MEDIDO (menu de idioma)**: host ~14,5 FPS (frame ~94 ms), relógio do jogo ~21 Hz de 50 (**42% do tempo real**). **A `GameThread` usa 99,6% de 1 núcleo de 16; a thread da janela, 4%.** Perfil de 25 s: **98,7% em `ps2_runtime`, quase tudo no rasterizador de GS em software (`GSCpuBackend`)**: `WritePixel` 17,7%, `SampleTexture` 22% (função + lambda), `lerpChannel` 7,9%, `DrawTriangle`+`DrawSprite` 12%, `GSMem::ReadTexture` 6%, `PixelStorageTraits::Read` 5,3%, `combineTexture` 5,1%, `bitsPerPixel` 4,2% (por pixel!). **O código recompilado do jogo é <0,5%.** Conclusão: o alvo da otimização é o GS em software, não a recompilação.
- Otimização do GS delegada (agente Opus) com regressão bit-a-bit (`work/reference/menu_idioma.png`, MD5 `B7953D5ACC553BDF82402D1733F953D2`); ver `docs/gs-optimization.md`.

## Próximos passos
0. Terminar o build 2; rodar `launcher run` com `--fps 0` para ver se o boot chega a desenhar algo e medir o FPS bruto.
1. Compilar o C++ gerado (248 MB, 39 mil arquivos: usar unidade única/`single_file_output` ou build paralelo com cuidado de RAM) junto com `ps2xRuntime`.
2. Habilitar e compilar `ps2xRuntime` e `ps2xIOP`; compilar o C++ gerado + runtime e ver até onde o boot chega (crt0 → `main` em `0x1943e0`; sequência de boot do IOP em `iop-boot.md`).
3. Critério de decisão (recompilação estática vs decomp manual): ver `../../docs/similar-projects.md`. Riscos conhecidos: GS/VU1 do runtime são limitados (README do projeto); licença GPL-3.0 afeta o código que linkarmos.
