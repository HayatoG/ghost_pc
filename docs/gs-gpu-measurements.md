# Medições do GS (contador `PS2X_GS_FEATURES=1`) — ghost_pc

Medido em 03/11/2026 com `PS2X_GS_FEATURES=1 PS2X_PAD_CLOCK=host` e `pad_scripts/explore2.txt`
(`ps2launch run`, build `build_perf`). O contador imprime a janela dos **últimos 600 presents**.

> **Ressalva:** a janela impressa no fim da rodada cobre o trecho em que o `explore2` já tinha
> voltado ao menu de idioma (screenshot `s09`). Então são números de **menu/LOADING**, não de
> gameplay. Repetir em outras fases antes de tirar conclusões gerais.

## Resumo da janela (600 presents, 299 844 draws, ~1,48 G pixels)

| Item | draws | pixels |
|---|---|---|
| Primitiva: triângulo / sprite | 51,45% / 47,04% | 37,46% / 62,53% |
| Sem textura / com textura | 60,06% / 39,94% | 54,39% / 45,61% |
| Textura T4 / T8 / CT32 | 22,87% / 10,29% / 6,78% | 9,13% / 6,44% / 30,03% |
| Textura lida de **alvo renderizado** (render-to-texture) | 6,77% | **29,99%** |
| CLUT recarregada | 18,75% | 10,41% |
| Filtro linear | 29,65% | 39,16% |
| Z16S com escrita | 93,22% | 69,97% |
| Frame CT32 / CT24 | 93,22% / 6,78% | 69,97% / 30,03% |
| Transferências host→local | 111 070 (≈105,8 Mpx) | — |
| Leituras local→host | 0 | 0 |

## Blend (ABE=1)

| Equação | draws | pixels | mapeamento |
|---|---|---|---|
| (Cs−Cd)·As+Cd, As≤0x80 | 29,95% | 40,45% | fixo |
| (Cs−Cd)·FIX+Cd, FIX<0x80 | 4,81% | 16,92% | fixo |
| (Cs−Cd)·As+Cd, As=textura | 22,87% | 9,13% | fixo* |
| (Cs−0)·As+Cd, As=textura | 10,29% | 6,44% | fixo* |
| (0−Cs)·As+Cd, As≤0x80 | 29,07% | 0,81% | fixo |
| (Cs−Cd)·As+Cd, As>0x80 | 0,81% | 0,00% | shader |

`fixo*` = vira blend fixo se o alfa da textura for ≤ 0x80. Total: **fixo 63,83% dos draws (58,17% px)**,
fixo* 33,16% (15,58% px), **shader só 0,81% dos draws (0,00% px)**. Conclusão: o blend do menu cabe quase
todo no blend fixo do D3D11; o caso "outro" (até 38% no levantamento anterior) é na prática o
`(0−Cs)·As+Cd` e `As=textura`, que são mapeáveis.

## Cobertura por fase do roteiro (`gs-gpu-step2-design.md`, seção 7)

| Fase | draws | % draws | % px | acumulado draws | acumulado px |
|---|---|---|---|---|---|
| E2 sprite sem textura + blend + Z16S | 88 452 | 29,50% | 26,04% | 29,50% | 26,04% |
| E3 + triângulo/Gouraud/fog/ATST | 87 088 | 29,04% | 28,35% | 58,54% | 54,38% |
| E4 + textura CT32/24/16 | 30 | 0,01% | 0,04% | 58,55% | 54,43% |
| E5 + CLUT T8/T4 | 99 431 | 33,16% | 15,58% | 91,71% | 70,00% |
| E6 + render-to-texture | 20 310 | 6,77% | 29,99% | 98,49% | 99,99% |
| resto (fica na CPU) | 4 533 | 1,51% | 0,01% | — | — |

Leituras:
- **E2+E3 já cobrem 54% dos pixels**, mas ainda sem textura; E4 praticamente não acrescenta (as texturas diretas CT32/24/16 são raras aqui).
- **O grosso da textura é CLUT (T4/T8): E5 leva a cobertura de pixels a 70%.**
- **Quase 30% dos pixels são render-to-texture (E6)**; sem E6 o ping-pong CPU↔GPU domina. Isso confirma o risco 2 do design.
- Zero leitura local→host na janela: o único readback é a apresentação (600 em 600 presents).

## Apresentação D3D11 (estágio 1) — bug corrigido

`PresentationFrame.pixels` vem do backend de referência num buffer fixo **640×512 com stride de
640 px**; `width×height` é só o retângulo válido no canto superior esquerdo. `GSGpuBackend::PresentOnGpu`
tratava o buffer como compacto (`width*4` por linha), o que cisalhava a imagem quando `width != 640`
(PAL: 560 de largura). Correção: textura 640×512, upload com pitch `640*4`, e `PresentTexture`
recorta a região válida (`gs_gpu_backend.cpp`). Verificado visualmente na janela D3D11 (frame de LOADING).

## Costuras pretas de 1 px a cada 64 px — corrigido no rasterizador de CPU

Sintoma (tela cinza de transição, ex.: após escolher o idioma): colunas pretas em x = 63, 127, 191…
Aparecem nas duas janelas (a de referência e a D3D11), porque vêm do frame do rasterizador, não da apresentação.

Causa (verificada com log dos vértices brutos): o jogo desenha a transição em **faixas de 64 px com
`x1 = x0 + 63,5`** (`x0=0,0 x1=63,5`, `64,0..127,5`, …). `GSCpuBackend::DrawSprite` truncava
(`static_cast<int>(63.5) = 63`) e cobria só `0..62`; o pixel 63 ficava com a cor de limpeza (preto).

Correção: cobertura de sprite `[ceil(x0), ceil(x1))` (centro do pixel em coordenada inteira) nos quatro
cantos de `DrawSprite` (`gs_cpu_backend.cpp`). `DrawLine` e a heurística de cópia de display em
`gs_frontend.cpp` não foram alterados.

Verificação: varredura de colunas escuras (`>50%` das linhas, 40 níveis abaixo dos vizinhos) em todos os
screenshots; antes `b11000.png` tinha 6 colunas, depois a tela cinza (`c10400.png`) não tem nenhuma.

**Novo MD5 de referência do menu de idioma** (`bench_menu_guest.txt`, `PS2X_GS=cpu`):
`a867491e5762ee08ee01a2e29f6bb8c3` (2 de 2 execuções idênticas). O anterior,
`b7953d5acc553bdf82402d1733f953d2`, estava **com as costuras** e deixa de valer. Diferença: 34 516 px
(≈10%); as colunas mais afetadas são 255/319/383 (as costuras) e o resto são bordas de sprites com
coordenada fracionária que deslocaram 1 px.

Pendente: confirmar com o PCSX2 (quando houver BIOS) que `ceil` para a borda esquerda também está certo.
A evidência atual a favor é só que as faixas de 63,5 do jogo ficam contíguas nesse modelo.

## Travamento intermitente após o LOADING (não reproduzido)

Uma execução interativa (modo `d3d11`, 03/11 ~21:10) parou em `WaitSema` (`pc=0x2ceda8`, chamador
`0x19c0e0` = `FUN_0019c0c8`) com o contador `gif` congelado em 1717; o `vsync` do guest seguia a 50 Hz.
O semáforo é o criado por `FUN_0019bfb4` (guardado em `gp-0x326c`) e sinalizado por `iSignalSema`
no handler `FUN_0019c11c` (fim de DMA). Antes do travamento o `vsync` ficou ~500 quadros do host parado e
depois saltou +18 de uma vez (a máquina estava ocupada com capturas de tela).
Tentativas de reproduzir: 5 execuções em `cpu`, 3 em `d3d11`, 1 `cpu` com 3 congelamentos de 8 s
(`NtSuspendProcess`) — **nenhuma travou**. Hipótese não confirmada: rajada de interrupções após congelar o host
faz o handler perder um `iSignalSema`.
Ferramenta deixada pronta: `PS2X_SEMA_TRACE=1` (create/wait/signal por id, com `pc` e `ra`; só os 40
primeiros por (op,id) + resumo a cada 2000). Também visto: `SignalSema id=-1` (11x, sempre de `ra=0x2ae904`)
e um RPC do IOP não tratado (`sid=0x0 rpc=0x4`, `pc=0x2ae8d4`) — não relacionados confirmados.
