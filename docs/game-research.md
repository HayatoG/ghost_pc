# Pesquisa específica — Ghost in the Shell: Stand Alone Complex (PS2)

Extraído de `docs/similar-projects.md` (levantamento de 2026-11-03). Itens não confirmados em fonte primária devem ser revalidados.

## Correção importante sobre o jogo

Fontes consultadas (TCRF, wiki do PCSX2) indicam que o desenvolvedor do jogo PS2 é a **Cavia**, não a Exact (a Exact fez o jogo de PS1 de 1997). Código do produto JP: SCPS-15064 (lançado em 04/03/2004). Revalidar antes de citar Exact/Sony em `docs/`. Não foi encontrado nenhum trabalho público de RE, tradução ou hack deste jogo, nem documentação de `IMAGE.BIN`, `BGM.BIN`, `RTDEN2EU.BIN` ou `MOV*.BIN` (ver seção 4).

## 4. Informação específica do jogo

- **Sem RE público encontrado.** Buscas por RE/hack/tradução do jogo, por `IMAGE.BIN`/`BGM.BIN`/`RTDEN2EU.BIN`/`MOV*.BIN` e por engine no GitHub e na web não retornaram nada. As páginas existentes cobrem só lançamento, protótipos (Hidden Palace: builds de 2004-09-02 e 2004-09-10, a segunda aparentemente com debug) e a ficha do PCSX2 wiki / TCRF. A página do Data Crystal retornou 403 e não foi lida.
- Os protótipos do Hidden Palace podem conter símbolos/strings de debug: se obtidos legalmente pelo usuário, seriam uma fonte valiosa de nomes (manter fora do git).
- **Formato de `IMAGE.BIN`**: um fórum (ZenHax) descreve `image.bin` do Drakengard com magic "fpk" e compressão LZ-like; é de outro jogo e outra empresa, **não aplicar sem verificar**; serve apenas como lembrete de padrão comum (cabeçalho de tabela + blocos comprimidos).
- Desenvolvedor: Cavia (ver correção no topo). Engine proprietária interna, sem documentação pública.
