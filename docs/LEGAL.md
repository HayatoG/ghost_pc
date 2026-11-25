# LEGAL — Ghost in the Shell: Stand Alone Complex (ghost_pc)

> **Estudo de caso / Case study.** PT-BR: projeto de estudo pessoal; o desenvolvimento só continua no meu tempo disponível, quando possível (sem prazos nem garantia de conclusão). EN: personal case study; development only continues in my available time, when possible (no deadlines, no guarantee of completion). Ver/see `README.md`.

> Orientação técnica do projeto, **não é aconselhamento jurídico**. Complementa `../../docs/LEGAL.md` (base). Repositório **público** desde 24/11/2026 por decisão do usuário (regra 3).

## Licença do código
- Este repositório usa **GPL-3.0** (`LICENSE`), porque os patches (`patches/ps2xRuntime-modified.patch`) e os arquivos novos em `patches/ps2xRuntime/` são derivados do PS2Recomp (GPL-3.0).
- Os arquivos de patch devem manter os avisos de copyright originais do PS2Recomp.

## Direitos autorais do jogo
- Jogo, nome, personagens e marca pertencem aos titulares originais. Este projeto **não é afiliado nem endossado por eles**.
- **Nada do jogo é versionado**: ISO, `SLES_530.20`, `MODULES/*.IRX`, `IMAGE.BIN`, `BGM.BIN`, `RTDEN2EU.BIN`, `MOV*.BIN`, texturas, áudio, vídeo e capturas de tela (`.gitignore` do repo bloqueia esses tipos).
- O launcher **exige o dump original do usuário** e valida serial `SLES_530.20` e o sha256 do ELF antes de rodar (regra 4). Os assets são extraídos para `work/`, ignorado pelo git.
- O código C++ gerado pela recompilação fica em `work/` e **não é versionado**.

## Conteúdo versionado (resumo)
Endereços, nomes de símbolos, formatos de arquivo documentados com palavras nossas, ferramentas, scripts de gamepad virtual, patches do runtime e documentação. Sem bytes do jogo.

## Antes de publicar
Seguir o checklist da seção 4 de `../../docs/LEGAL.md` e, adicionalmente: conferir que `docs/*.csv` e `docs/*.md` contêm só nomes/endereços (sem dumps de bytes) e que `reference/` e `shots/` não estão no histórico.
