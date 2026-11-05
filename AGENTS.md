# ghost_pc — Ghost in the Shell: Stand Alone Complex (PS2, Europa)

Tudo que é **específico deste jogo** mora aqui. Ferramentas e regras genéricas ficam na raiz (`../AGENTS.md`, `../tools/`, `../docs/`) e valem também neste diretório. Leia a raiz primeiro.

## Alvo

| Item | Valor |
|---|---|
| Desenvolvedora (PS2) | Cavia |
| Imagem (dump do usuário) | `Ghost in the Shell - Stand Alone Complex (Europe) (En,Fr,De,Es,It).iso` (~4,6 GB, DVD) — fica aqui, **não versionada** |
| Executável de boot | `SLES_530.20` (3 742 404 bytes, LBA 850001), sha256 `9ddec62618d6770261ae87cb53eec04552d8f67ba794e3c5e8689b7523e258f7` |
| Vídeo | PAL |
| SDK | Sony 2.8.0 (`IOPRP280.IMG`, strings `PsIIlib* 2800`) |
| Arquivos do disco | `MODULES/` (IRX + IOPRP280.IMG), `MOV00EU.BIN`, `MOV01EU.BIN`, `MOV02EU.BIN`, `MOV00EUS.BIN` (vídeo), `RTDEN2EU.BIN`, `BGM.BIN` (áudio), `IMAGE.BIN` (gráficos), `DUMMY.BIN` |
| Projeto Ghidra (ReVa) | `/SLES_530.20`, linguagem `r5900:LE:32:default` |

## Layout

```
ghost_pc/
├─ AGENTS.md          # este arquivo
├─ *.iso              # dump — NÃO versionado
├─ work/              # extraído do dump (ELF, IRX…) — NÃO versionado
├─ logs/              # logs das ferramentas — conteúdo NÃO versionado
├─ docs/              # notas de RE deste jogo (módulos IOP, símbolos, formatos, mapa de memória)
├─ src/               # código decompilado/reescrito e camada de port deste jogo
└─ launcher/          # launcher deste jogo
```

## Launcher (regra 5)

`game.toml` (neste diretório) descreve o jogo para o launcher genérico da raiz: serial, sha256 do ELF de boot, arquivos a extrair e, quando o port existir, o executável. Comandos (da raiz do repo):

```
python -I launcher/ps2launcher.py info    --game ghost_pc
python -I launcher/ps2launcher.py verify  --game ghost_pc   # exige o dump original (serial + hash)
python -I launcher/ps2launcher.py extract --game ghost_pc   # SYSTEM.CNF, SLES_530.20, MODULES -> work/
python -I launcher/ps2launcher.py run     --game ghost_pc   # verify + extract + inicia o port (precisa de [run].executable)
```

Logs em `logs/launcher_<data>.log` e `logs/run_<data>.log`. Preencher `[run]` quando o runtime estiver construído.

### Launcher em Rust e opcoes de execucao

`launcher-rs/` (`ps2launch` CLI, `ps2launch-gui`) le `launcher.cfg` e exporta variaveis `PS2X_*` ao runtime. Precedencia: **linha de comando > ambiente do processo > `launcher.cfg` > `game.toml`**. Padrao (perfil `recommended`): `hw`, escala 1, CRT desligado, cache automatico; `deck` = `cpu-thread`; `original` = `cpu` serial. Renderizadores: `hw` (GPU em thread, o mais rapido), `cpu-thread`, `cpu`; avancado `d3d11`, `hw-serial`, `cpu-bands`. Opcoes (tabela completa em `launcher-rs/README.md`): `--renderer` (`PS2X_GS`), `--internal-scale 1..8` (`PS2X_GS_SCALE`, so hw), `--crt [off|suave|medio|forte]` + `--crt-strength 0..100` (`PS2X_POSTFX_CRT`/`_STRENGTH`), `--vfs-cache-mb N` (`PS2X_VFS_CACHE_MB`, 0 desliga, ausente = automatico), `--bands N|auto|off` (`PS2X_GS_BANDS`, so cpu-thread/cpu-bands, nunca com hw), `--queue-kb N` (`PS2X_GS_QUEUE_KB`), `--vsync`, `--fps`/`--fps-limit`, `--widescreen` (`PS2X_WIDESCREEN`, planejado, sem efeito). Se o `hw` nao iniciar o runtime loga `[gs-hw] ... using cpu`; o launcher avisa e o CLI tenta `cpu-thread` se o hw fechar com erro nos primeiros 20 s. O launcher registra RAM/VRAM no log e avisa (sem bloquear) em escala > 2 ou cache grande com pouca memoria. A aba Imagem da janela expoe tudo isso.

## Como rodar as ferramentas da raiz

Rode **a partir de `ghost_pc/`** para que `work/` e `logs/` sejam relativos ao jogo:

```
cd ghost_pc
python -I ../tools/extract_iso.py "<iso>" work MODULES SYSTEM.CNF SLES_530.20
python -I ../tools/iop_modules.py work/MODULES --json logs/iop_modules.json
```

## Estado

- [x] ELF extraído, importado e analisado (7462 funções). crt0 em `0x00100008`; `FUN_001943e0` = provável `main`.
- [x] Módulos IOP catalogados (`docs/iop-modules.md`).
- [x] Nomes do SDK: 168 funções em `docs/symbols.csv` (alta/média/baixa confiança); aplicados também no projeto headless `work/ghidra_project`.
- [x] Os `.BIN` grandes usam um contêiner `fpk` (magic `fpk\0`, tabela de entradas com offset/tamanho alinhados a 0x800) — parser e doc em andamento (`tools/fpk.py`, `docs/fpk-format.md`).
- [x] **O jogo roda no port (03/11, ~05:20)**: boot do IOP, `fioInit`/CD ok, interrupções de DMA ok (latch), display configurado, **menu de idioma e abertura hexagonal desenhados**, **gamepad virtual funcional** (o pad anuncia DualShock). Sem alvos ausentes após a varredura de ponteiros (349 funções novas). Ver `docs/ps2recomp-spike.md` (rodadas 3–6) e `docs/gs-optimization.md`.
- [x] Desempenho medido (relógio do jogo vs. 50 Hz PAL): menu ~81% (era 42%), telas intermediárias 61–68%, **cena hexagonal 46%** (22,9 Hz). Gargalo = rasterizador de GS em software (98% do tempo da `GameThread`); código recompilado <0,5%.
- [ ] Otimizar o GS na cena hexagonal (meta ≥35 Hz, ideal 50); depois explorar as telas seguintes com o pad virtual (`pad_scripts/explore2.txt`) e tratar novos "muros" (ciclo: `tools/recomp_cycle.ps1 -PointerScan`, `PS2X_MISSING_LOG`).
- [ ] Decompilar `EZSOUND.IRX` (middleware da Cavia) e confirmar ordem de carga dos módulos por xref (em andamento: `docs/iop-boot.md`).
- [ ] Spike do PS2Recomp: exportação pronta (`work/recomp/ghost.toml`), **compilação pausada**; ver `docs/ps2recomp-spike.md`.
- Atenção: um agente atribuiu a engine à "Sega/Production I.G."; **não confirmado** (a pesquisa aponta a Cavia como desenvolvedora). Namespaces vistos nas strings: `abes` (cena, partículas, câmera) e `Stacom` (jogo).
