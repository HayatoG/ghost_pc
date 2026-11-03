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

## Próximos passos
0. Terminar o build 2; rodar `launcher run` com `--fps 0` para ver se o boot chega a desenhar algo e medir o FPS bruto.
1. Compilar o C++ gerado (248 MB, 39 mil arquivos: usar unidade única/`single_file_output` ou build paralelo com cuidado de RAM) junto com `ps2xRuntime`.
2. Habilitar e compilar `ps2xRuntime` e `ps2xIOP`; compilar o C++ gerado + runtime e ver até onde o boot chega (crt0 → `main` em `0x1943e0`; sequência de boot do IOP em `iop-boot.md`).
3. Critério de decisão (recompilação estática vs decomp manual): ver `../../docs/similar-projects.md`. Riscos conhecidos: GS/VU1 do runtime são limitados (README do projeto); licença GPL-3.0 afeta o código que linkarmos.
