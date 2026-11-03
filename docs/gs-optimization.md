# Otimização do rasterizador de GS em software (GSCpuBackend)

Data: 2026-11-03. Escopo: `work/ps2recomp_game/ps2xRuntime/src/lib/gs/*` e `include/runtime/gs/*`
(PS2Recomp, GPL). Backups dos originais: `work/backup_gs/` (16 arquivos, copiados antes da 1ª edição).

## Resultado

Benchmark: `launcher run --fps 60 --pad-script ghost_pc/pad_scripts/bench_menu.txt` (660 quadros de
jogo; captura no quadro 600). Regressão: `logs/shots_bench/bench_menu.png` **byte-idêntica** a
`work/reference/menu_idioma.png` (MD5 `B7953D5ACC553BDF82402D1733F953D2`) em **todas** as etapas.

| Etapa | guest_vsync_hz (corrida inteira) | realtime_pct | Menu (CSV, regime) | MD5 |
|---|---|---|---|---|
| Baseline (exe de 04:25) | 19,01 | 38,0 % | ~21 Hz (medição do pai) | OK |
| 1. Setup por primitiva + `roundToU8` | 25,10 | 50,2 % | — | OK |
| 2. `__forceinline` nos helpers | 26,76 | 53,5 % | — | OK |
| 3. Bilinear SIMD + leitura CT32 inline | 30,27 | 60,6 % | — | OK |
| 4. Swizzle CT32/CT24/Z16S inline | 32,14 | 64,3 % | — | OK |
| 5. Snapshot de página de textura adiado | 31,73 | 63,5 % | ~37 Hz | OK |
| 6. `__forceinline` de WritePixelFast/SampleTextureFast | **34,53** | **69,1 %** | **~40,5 Hz (81 %)** | OK |

A métrica "corrida inteira" inclui a fase de logos/intro (t≈4–11 s, ~18 Hz, ainda pesada) e o boot;
no menu de idioma o regime estável (CSV `logs/bench.csv`, últimos ~15 s) ficou em **~40,5 Hz**
(era ~21 Hz). Meta de 50 Hz **não** atingida. FPS do host no menu: 20–40 (alterna; ver CSV).

Perfil final (25 s no menu): `DrawSprite` 65 % (com WritePixel/SampleTexture inlined),
`DrawTriangle` 27 %, leitor PSMT4 genérico 2,6 %, resto < 1 %. Antes: WritePixel 17,7 %,
SampleTexture+lambda 22 %, lerpChannel 7,9 %, bitsPerPixel 4,2 % etc.

## Mudanças (e por quê)

1. **`PixelSetup` / `TextureSetup` por primitiva** (`gs_cpu_backend.cpp`, decl. em `gs_cpu_backend.h`).
   `WritePixel` redecodificava FRAME/ZBUF/TEST/ALPHA/FBA, chamava `bitsPerPixel` (switch) e indexava a
   tabela de ponteiros de função a cada pixel; `SampleTexture` redecodificava CLAMP/TEX0 e fazia
   switch de psm por texel. Agora `PreparePixelSetup`/`PrepareTextureSetup` resolvem isso uma vez por
   primitiva e `WritePixelFast`/`SampleTextureFast`/`SampleTexelFast` repetem *exatamente* a mesma
   lógica (mesma ordem de testes: scissor → fog → ATST/AFAIL → leitura RMW → DATE → ZTST → blend →
   FBA → FBMSK → preserve-alpha → 16 bits → escrita; Z por último). `DrawSprite` e `DrawTriangle` usam
   o caminho rápido; `DrawLine`/ponto continuam no `WritePixel` original (intocado).
   `GSMem::GetReadTextureFunc(psm)` devolve o leitor tipado (mesmo resultado de `ReadTexture`).
2. **`lerpChannel` sem `std::lround`**: `roundToU8` (trunca + fração ≥ 0,5) é idêntico a
   `clampU8(lround(x))` para a faixa produzida pelo filtro (combinação convexa de 0..255; qualquer
   resultado ≤ 0 vira 0 de qualquer forma; NaN → 0 nos dois). Elimina uma chamada de CRT por canal.
3. **`GS_FORCEINLINE`** em helpers pequenos (applyTexa, wrap, combineTexture, conversões 5551) e,
   na etapa 6, em `WritePixelFast`/`SampleTextureFast`: o MSVC não os inlinava e pagava chamada
   com 6–9 argumentos por pixel; inlinados nos laços, as leituras do setup viram registradores.
4. **Bilinear em SIMD (SSE4.1/FMA)**: desmontei o código escalar (`dumpbin /disasm`) e confirmei que,
   com `/fp:fast /arch:AVX2`, `lerpChannel` compila para 3 `vfmadd231ss` (top, bottom, vertical). O
   caminho novo faz a *mesma* sequência (`_mm_fmadd_ps`) nos 4 canais de uma vez, mais o mesmo
   arredondamento e clamp (`packs`/`packus` saturam igual a `clampU8`). Fixar as intrínsecas também
   tira a dependência de o compilador decidir contrair ou não.
5. **Endereço de swizzle inline** (`swizzledByteAddress<psm>`): cópia force-inline de
   `PixelStorageTraits::Address` + máscara de `Read`/`Write`, em aritmética mod 2^32 como o original.
   Usado para textura PSMCT32, framebuffer CT32/CT24 (CT24 com o mesmo RMW do byte alto de
   `WriteCT24`) e Z16S (o Z do menu). `GSMem::GetPageTableC32/Z16S()` expõem as tabelas.
   Os demais formatos seguem pelos ponteiros de função originais.
6. **Snapshot adiado no `TexturePageCache`** (`gs_texture_page_cache.h`). O cache de 1 página copia
   8 KB a cada *miss*; no bilinear, linhas/colunas que cruzam a borda de página alternam A/B/A/B e
   copiavam 16 KB por pixel. Novo `ResolveDeferred`: num miss só marca a página como "pendente"
   (snapshot == VRAM viva enquanto ninguém escrever nela) e lê da VRAM; `NotifyWrite(addr)` antes de
   cada escrita conhecida materializa a cópia se a escrita cair na página pendente; escritas por
   ponteiro de função (endereço desconhecido) materializam antes; `DrawSprite`/`DrawTriangle`
   materializam ao sair, então o estado pendente nunca vaza para LoadClut/Upload/TextureFlush etc.
   `Resolve` (caminho antigo) trata o estado pendente. Resultado observável idêntico por construção
   (feedback textura↔framebuffer continua vendo o snapshot antigo como antes). Ganho medido
   pequeno na média global, mas p50 do quadro caiu de 56 → 38 ms.

## Tentado / não feito

- Flags de compilação: não mexidas (fora do escopo de arquivos e sem medição que justificasse).
- Paralelizar sprites/triângulos em bandas: **não feito**. O cache de página de textura é estado
  compartilhado e sequencial (sua semântica de snapshot depende da ordem exata de leituras e
  escritas), e há primitivas que leem o próprio framebuffer (blend/DATE) e possivelmente texturas
  sobrepostas ao alvo. Exigiria provar ausência de sobreposição por primitiva e um cache por thread
  com a mesma semântica — não cabia com segurança no tempo.
- Inverso de `spriteW` (multiplicar em vez de dividir) e outras reassociações de float: descartado,
  não é bit-exato.

## Riscos

- O bilinear SIMD fixa FMA. A referência foi gerada com o binário que já usava FMA (confirmado no
  disassembly); se alguém compilar o runtime **sem** `/arch:AVX2`/`/fp:fast` (escalar sem FMA), o
  caminho antigo daria resultados diferentes do novo — o novo exige AVX2/FMA na CPU.
- `swizzledByteAddress` duplica a lógica de `PixelStorageTraits::Address`; mudanças futuras lá
  precisam ser espelhadas.
- Toda escrita de VRAM nova dentro de `DrawSprite`/`DrawTriangle` precisa chamar
  `NotifyWrite`/`Materialize` (comentado no header).
- Só uma captura de regressão (menu de idioma); caminhos não exercitados por ela (fog, DATE,
  AFAIL, CLUT CSM2, Z24/Z32) foram portados linha a linha, mas não têm teste de imagem.

## Fase 2 — cena hexagonal da abertura (05:20–05:50)

Medição com o binário da fase 1 (sem mudanças de código nesta fase):
`launcher run --pad-script ghost_pc/pad_scripts/explore2.txt --fps-log ghost_pc/logs/hex_base.csv
--timeout 200`. Nesta corrida a cena hexagonal (captura `logs/shots_hex_base/s06.png`: hexágonos
ciano nítidos sobre ruído ciano/laranja) só entra por volta de t≈182 s e fica estável em
**~23,9 Hz de relógio de jogo / ~24 FPS no host** (t 183–199 s). Antes dela: t≈139–151 s ~36 Hz
de jogo (host 18 FPS), t≈167–178 s 12,4 Hz (outra fase pesada da intro).
Atenção: com `--timeout 200` a cena mal aparece; para medir 60 s nela use `--timeout 260` ou mais.

Perfil (PS2X_PROFILE_DELAY=130, 25 s, pad explore2): `DrawSprite` 53,9 %, **`DrawTriangle` 40,8 %**,
leitor PSMT4 genérico 1,4 %, `NotifyWrite` 0,8 % (não foi inlinado), `WriteP4` 0,6 %. (Esta
amostra rodou em paralelo com outra instância do jogo; as porcentagens relativas valem, o tempo
absoluto não.)

**Nenhuma mudança de código foi entregue nesta fase.** No prazo de ~25 min, cada ciclo
(build 2 min + regressão do menu 1 min + corrida até a cena ~3,5 min) só permitia uma tentativa,
e as candidatas bit-exatas que cabiam (templates por combinação de modos, LUT do alpha test,
`NotifyWrite` inline) teriam ganho estimado de 2–10 %, longe da meta de 35 Hz, sem tempo de
validar. Preferi não deixar código não medido na árvore: o estado atual é o da fase 1
(regressão do menu OK).

O que provavelmente leva a cena a ≥ 35 Hz (por ordem de ganho esperado):
1. **Paralelismo por bandas de linhas** dentro de cada primitiva grande (16 núcleos). Para ser
   bit-exato: só quando as páginas de FRAME/ZBUF da primitiva não se sobrepõem às páginas da
   textura (então o cache de página nunca vê escrita relevante e cada thread pode usar leitura
   direta da VRAM); cada pixel é independente dentro de uma primitiva (sprite/triângulo não
   escrevem o mesmo pixel duas vezes), então bandas disjuntas dão a mesma VRAM final. Juntar as
   threads ao fim de cada primitiva preserva a ordem entre primitivas.
2. **Triângulo por varredura de linhas com o mesmo teste**: hoje a caixa envolvente inteira é
   testada pixel a pixel; dá para manter o teste exato (mesmas expressões em float) e só pular o
   trabalho de cor/textura/escrita, que já é o que acontece — o ganho real vem de especializar o
   laço por (tme, iip, fst, bilinear, blend, z) com templates.
3. Leitores inline para PSMT4/PSMT8 (CLUT) como já feito para CT32.

## Próximos passos sugeridos

- A fase de logos/intro (~18 Hz) merece um perfil próprio.
- Especializar o laço de sprite por linha (parte do endereço dependente de y constante por linha;
  alpha test por tabela de 256 entradas) e templates por combinação (sem Z / sem blend).
- Paralelismo por bandas com verificação de não-sobreposição textura/FRAME/ZBUF por primitiva.

## Fase 3 (agente pai, 05:30): experimento negativo — LTCG só no runtime
- **Hipótese:** o rasterizador está na biblioteca `ps2_runtime` (~30 arquivos); ligar `/GL` + `/LTCG` só nela (o código gerado, que estourava a pilha do compilador sob LTCG, ficaria de fora) permitiria inlining entre arquivos.
- **Resultado:** build em 2 min, sem estouro de pilha, saída **byte-idêntica** (MD5 `B7953D5ACC553BDF82402D1733F953D2`), mas **sem ganho**: 34,9 e 34,3 Hz do relógio do jogo contra 34,6 Hz sem LTCG (ruído). O custo está no trabalho por pixel, não em chamadas entre arquivos.
- **Decisão:** revertido (configuração mais simples, mesmo desempenho). Os próximos ganhos têm de vir de algoritmo: especializar `DrawSprite`/`DrawTriangle` por combinação de modos, SIMD de vários pixels por vez e, só com prova de saída idêntica, paralelismo por bandas.

## Estado final desta janela (03/11, 05:36)
| Cena | Relógio do jogo (alvo 50 Hz) |
|---|---|
| Menu de idioma (boot+logos incluídos), antes | 19,0 Hz (38%) |
| Menu de idioma, depois | 34,7 Hz (69%) — média; o menu parado chega a ~40 Hz (81%) |
| Tela parada logo após o idioma | 50,0 Hz (99,99%), 52 FPS no host |
| Abertura hexagonal (a mais pesada medida) | ~23–24 Hz (46–48%) |
Perfil da cena hexagonal (agente Opus): `DrawSprite` 53,9%, `DrawTriangle` 40,8%, resto <1,5%.
