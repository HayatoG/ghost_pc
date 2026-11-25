# Architecture — Ghost in the Shell: Stand Alone Complex (ghost_pc)

> **Estudo de caso / Case study.** PT-BR: projeto de estudo pessoal; o desenvolvimento só continua no meu tempo disponível, quando possível (sem prazos nem garantia de conclusão). EN: personal case study; development only continues in my available time, when possible (no deadlines, no guarantee of completion). Ver/see `README.md`.

Complementa `../../docs/ARCHITECTURE.md` (base genérica). Aqui só o que é específico deste jogo.

## Identidade
- PS2, Europa (PAL), serial `SLES_530.20`; o launcher valida o serial (BOOT2) e o sha256 do ELF
  (`game.toml`). Região PAL: o launcher define `PS2X_VIDEO_HZ=50`.
- SDK Sony 2.8.0. Contêiner de dados "fpk"; compressão V3a (LHA lh5); texturas "zim" baseadas em
  pacotes GS (`fpk-format.md`, `v3a-zim-format.md`).

## Arquivos do jogo (resumo)
| caminho | o que guarda |
|---|---|
| `game.toml` | serial, região, hash do ELF, lista de extração, executável e ambiente de execução |
| `patches/` | `ps2xRuntime-modified.patch` + cópias dos arquivos novos do runtime (pad virtual, estatísticas de frame, relógio de vídeo, estatísticas de IRQ, profiler, `ghost_overrides.cpp`, teste do pad) |
| `pad_scripts/` | scripts de gamepad virtual: `smoke`, `explore`, `explore2`, `nav`, `input_test`, `hold_test`, `peek_test`, `bench_menu`, `boot_to_menu` |
| `docs/` | `iop-modules`, `iop-boot`, `fpk-format`, `v3a-zim-format`, `game-research`, `dma-irq-analysis`, `research-dma-irq`, `gs-optimization`, `ps2recomp-spike`, tabelas de símbolos (`symbols*.csv`) |
| `work/` | **não versionado**: arquivos extraídos, projeto Ghidra, C++ gerado, árvore de build, imagens de referência |

## Particularidades do jogo no runtime
- **Pad:** só aceita DualShock analógico (id `0x73`); `scePadInfoMode` precisa anunciá-lo.
- **DMA:** depende do latch de conclusão do DMAC (aviso antes do handler registrado).
- **Overrides** (`patches/ps2xRuntime/src/ghost_overrides.cpp`), registrados para
  `ghost_in_the_shell_sac_pal` / `SLES_530.20`: `fioInit` e `fioIoctl` retornam 0;
  `_sceCdSCmd` e `_sceCdNCmd` retornam 1 (endereços no próprio arquivo).
- **Funções só alcançadas por ponteiro** (tabelas de dispatch) não eram vistas pelo Ghidra; a
  varredura de ponteiros (`tools/ghidra/CreateFunctionsFromPointers.java`, via
  `recomp_cycle.ps1 -PointerScan`) acrescentou 349 funções e eliminou as paradas por "alvo ausente".
- **Vídeos:** o jogo procura `MOV*.BIN` no disco; eles são extraídos para `work/` para a busca de
  arquivos funcionar. A reprodução (FMV) não foi tratada.

## Estado (resumo; números em `ps2recomp-spike.md` e `gs-optimization.md`)
Chega à tela de título e à abertura com o pad virtual. Desempenho medido pelo relógio do jogo
contra 50 Hz: menu ~69–81%, abertura hexagonal ~46%. Gargalo: rasterizador GS por software.
Áudio, FMV e memory card: não tratados.
