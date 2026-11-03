# ghost_pc — Ghost in the Shell: Stand Alone Complex (PS2) → PC

Port do jogo (PS2, versão Europa `SLES_530.20`) por recompilação estática. Repositório do jogo; a base genérica está em **ps2_recomp** (`HayatoG/ps2_recomp`).

> **Privado e sem conteúdo do jogo.** Nada de ISO, ELF, texturas, áudio ou vídeo: só código, patches, notas de engenharia reversa (endereços, nomes, formatos) e scripts. É preciso o **dump original** (`*.iso` nesta pasta); o launcher confere serial e sha256 do ELF de boot.

## Conteúdo
| Pasta/arquivo | O quê |
|---|---|
| `AGENTS.md` | Alvo, layout, estado e como rodar as ferramentas |
| `game.toml` | Configuração do launcher (serial, hash do ELF, arquivos a extrair, executável, região PAL) |
| `docs/` | Notas de RE: módulos IOP, sequência de boot do IOP, contêiner `fpk`, `V3a` e `zim`, nomes de funções (`symbols*.csv`), spike do PS2Recomp |
| `patches/` | Alterações ao runtime do PS2Recomp (pad virtual, medidor de FPS, capturas de tela, vblank por região, opção sem LTCG, registro de alvos ausentes) |
| `pad_scripts/` | Scripts de entrada do gamepad virtual (por quadro) |

## Como rodar (a partir da raiz do repositório genérico)
```
python -I launcher/ps2launcher.py verify --game ghost_pc
python -I launcher/ps2launcher.py run --game ghost_pc --fps 60 --pad-script ghost_pc/pad_scripts/smoke.txt --shot-dir ghost_pc/logs/shots
```

## Estado
O recompilador converte o ELF inteiro e o runtime abre a janela, mas o jogo **ainda não desenha** (quadro magenta). Detalhes e próximos passos em `AGENTS.md` e `docs/ps2recomp-spike.md`.

## Licença dos patches
Os arquivos em `patches/` são derivados do PS2Recomp e seguem a **GPL-3.0**.
