# Regressao visual (capturas de referencia do baseline)

Objetivo: provar que otimizacoes (VU1, rasterizador, threads do GS, backend GPU) nao mudam a
imagem. Ferramentas: `tools/image_regress.py` (raiz, generica) e `ghost_pc/tools/regress.ps1`
(roda o jogo e compara).

## Comando

```
cd ghost_pc
powershell -NoProfile -File tools\regress.ps1 -Scenario menu                  # ~25 s
powershell -NoProfile -File tools\regress.ps1 -Scenario scene3d               # varios minutos
powershell -NoProfile -File tools\regress.ps1 -Scenario menu -Exe <outro.exe> # outro executavel
```

- `-Exe` padrao: `work\ps2recomp_game\out\build_perf\ps2xRuntime\ps2EntryRunner.exe` (as DLLs
  precisam estar ao lado do exe). O baseline congelado e `work\baseline_exe\ps2EntryRunner.exe`.
- Saida: 0 = igual; 1 = diferenca (ver `logs/regress_<cenario>_*.log|jsonl`); 2 = falha de execucao
  (timeout, captura faltando). Capturas do candidato ficam em `work\regress_out\<cenario>_<carimbo>`
  (apagadas se igual, a menos de `-KeepOut`). Logs: `logs/regress_*.log`, `logs/regress_run_*.log`.
- Tolerancia: `-MaxErr N` (erro maximo por canal), `-MaxDiffPx N`, `-MinPsnr dB`; padrao tudo 0 =
  pixels identicos. Use tolerancia so para backend GPU (ordem de arredondamento), nunca para
  mudancas que deveriam ser exatas (VU1, threads).
- `-Renderer cpu|d3d11` (padrao cpu: a referencia e do rasterizador de CPU). `-PadClock guest|host`.
- Cenarios: `menu` = `pad_scripts/bench_menu.txt` (1 captura, vsync 600); `scene3d` =
  `pad_scripts/regress_scene3d.txt` (botoes de `scene3d.txt`, 6 capturas ate o vsync ~7700).

## Ambiente que o script fixa

`PS2X_PAD_CLOCK=guest` (script em vsyncs do jogo, nao em quadros do host), `PS2X_TARGET_FPS=0`
(sem limite; rapido), `PS2X_VIDEO_HZ=50`, `PS2X_GS=cpu`, `PS2X_PAD_SCRIPT`, `PS2X_SHOT_DIR`.
Executavel chamado direto (sem `ps2launch`, que nao aceita trocar o exe; ele le `[run].executable`
do `game.toml`), com `work\SLES_530.20` como argumento e cwd = `ghost_pc`.

## Determinismo

Medido em 04/11: o baseline rodado duas vezes (cada cenario, rasterizador de CPU) gera capturas
**byte-identicas** (mesmo MD5 de arquivo): `menu` (1 quadro) e `scene3d` (6 quadros), referencia
vs. segunda rodada = 0 pixels diferentes.

Por que e reproduzivel: com `PS2X_PAD_CLOCK=guest` o pad script e o `shot` sao disparados pelo
contador de vsync emulado (`gs().vsyncTick`), nao pelo numero de quadros do host nem pelo tempo de
parede; o jogo mede tempo pelo vsync emulado e o rasterizador de CPU e deterministico. Com
`PS2X_PAD_CLOCK=host` os scripts dependem da velocidade da maquina (nao usar para regressao).
Fonte residual de risco: a captura le a textura apresentada no quadro do host em que o vsync
alcanca o alvo; se um build novo fizer o vsync saltar mais de 1 entre dois quadros do host, a
captura pode sair 1 vsync depois (diferenca legitima de temporizacao, nao de desenho). Num build
muito mais rapido/lento o resultado do jogo continua o mesmo por vsync, entao diferencas de
temporizacao so aparecem se o GS ficar assincrono em relacao ao vsync (threads do GS): nesse
caso, investigar com `-Renderer cpu` e comparar quadros vizinhos antes de culpar o desenho.

## Regerar a referencia

Somente com o baseline congelado (ou apos decisao consciente de mudar o padrao):

```
powershell -NoProfile -File tools\regress.ps1 -Scenario menu    -Exe work\baseline_exe\ps2EntryRunner.exe -Record
powershell -NoProfile -File tools\regress.ps1 -Scenario scene3d -Exe work\baseline_exe\ps2EntryRunner.exe -Record -TimeoutSec 900
```

PNGs de referencia ficam em `work/reference_regress/` (ignorado pelo git; contem imagem do jogo,
nunca versionar).
