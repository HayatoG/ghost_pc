# Direção de arte e prompts para gerar a capa do atalho (uso pessoal)

Este documento descreve, **com palavras nossas**, o visual do jogo, a partir de uma varredura das
~10,9 mil texturas do `IMAGE.BIN` (convertidas localmente em `work/art_all/`, que não é versionada).
Não há pixels, nomes de arquivos nem texturas do jogo aqui; só estilo, paleta, composição e
prompts. Uso: o dono do dump gera uma arte própria com outras IAs e o launcher a recorta para o
atalho da Steam (`../../docs/steam-library.md`). Uso pessoal; ver `LEGAL.md`.

## 1. O que o jogo mostra (resumo da análise)

O que existe no disco **não é arte de caixa**: são texturas de jogo. As famílias vistas:

| Família | O que é | Valor para a capa |
|---|---|---|
| Telas de jogo / carregamento (264x200, 272x160, 440x312) | Cenas de nível: porto de contêineres à noite com guindastes, um poço circular com um círculo de céu azul no topo, túnel de dutos e tubulações, planície costeira com colinas sob céu azul. Um quadro (440x312) mostra a heroína de costas atirando em um corredor de contêineres | Melhor referência de **ambientação e composição** |
| Personagem | Traje justo preto (couro/látex sintético) com detalhes, versões em vermelho; texturas de pele, tatuagem e cabelo; botas altas; protagonista feminina ciborgue, cabelo escuro curto | Referência de **figurino** |
| Armas | Placas de metal gasto verde-acinzentado, com rebites, e a silhueta de cada arma gravada (fuzis de assalto, submetralhadora, rifle de precisão, lançador, dispositivo) | Referência de **moldura/ícone** |
| HUD / UI | Ciano e azul-petróleo sobre cinza; anéis e arcos segmentados; molduras com traços de circuito e "código de barras"; colchetes e mira circular | Referência da **linguagem gráfica** |
| Abertura | Raios de luz verde-azulada convergindo para um ponto central sobre fundo escuro com malha/circuitos | Referência de **efeito** |
| Ambiente | Nuvens volumétricas macias, sol/halo difuso, silhueta de cidade ao crepúsculo, colinas, mariposa/asas, chamas e fumaça em folhas de sprite | Referência de **céu e atmosfera** |
| Logos de estúdio | Logotipos de duas empresas na abertura | **Não usar** (marcas de terceiros) |

Linguagem visual: **ficção científica cyberpunk "tecnológica-industrial"** (instalações portuárias
e subterrâneas, metal envelhecido, sujeira, neon discreto), com **HUD ciano** e uma heroína
ágil, de silhueta escura, contra ambientes frios e azulados. Cenas de ação noturnas, com explosão
de luz de cano de arma; o quadro geral é de **terceira pessoa por trás do ombro**.

### Paleta
- Base: preto-azulado, cinza-chumbo, azul-aço frio.
- Destaque de HUD/tecnologia: **ciano / azul-petróleo** (#2fd0e0 a #1b6f86) e verde-água.
- Acentos: laranja-quente de fogo/farol e **vermelho** sangue/alerta; amarelo-sujo industrial.
- Céu de dia: azul médio com nuvens brancas; crepúsculo: azul-marinho a laranja fraco.

### Elementos recorrentes para citar nos prompts
Contêineres empilhados com ferrugem e grafismos laranja/vermelhos, guindastes de porto, poças e
asfalto molhado, dutos e canos, grades, painéis metálicos rebitados, HUD circular segmentado em
ciano, linhas de circuito, código de barras, luz volumétrica, faíscas e clarão de tiro.

## 2. Prompts para outras IAs

Dica: IAs de imagem seguem melhor **inglês**. Geramos a base uma vez e depois variamos o formato.
Peça fundo **sem texto** (o launcher/Steam coloca o nome) e logos **separados** em fundo
transparente, se a ferramenta permitir.

### 2.1 Prompt-base (cena)

```
Cinematic key art for a near-future cyberpunk third-person action game, original illustration.
A slim female cyborg operative with short dark bob hair, seen from behind and slightly to the side,
wearing a sleek black tactical bodysuit with subtle cyan glowing seams and tall black boots,
firing an assault rifle with a bright muzzle flash. Night-time industrial shipping-container port:
towering stacked rusty containers with orange and red stencil markings, a gantry crane silhouette,
wet asphalt reflecting cold light, drifting steam, scattered sparks. Cold blue and steel-grey
palette with cyan holographic HUD elements (segmented circular reticle, thin circuit-trace frames,
barcode-like details) floating at the edges, warm orange accent light from a distant lamp.
Volumetric light, dramatic rim lighting, shallow haze, high detail, painterly sci-fi concept art,
sharp focus on the character, strong depth, no text, no watermark, no logos.
```

Negativo: `text, letters, watermark, logo, signature, extra limbs, deformed hands, blurry, low
resolution, cartoon, anime chibi, cluttered UI, brand names`.

### 2.2 Variações por formato (mesmo estilo, composição diferente)

- **Capa vertical (cartaz, proporção 2:3, ex.: 600x900):** a mesma cena com a heroína ao centro e
  grande, céu e luzes no alto; deixe o terço de cima mais vazio (espaço para o nome no Steam).
  Acrescente: `vertical movie-poster composition, character centered, empty dark sky at the top`.
- **Grade larga (cerca de 920x430, ~2,1:1):** `wide cinematic banner composition, character on the
  left third, container canyon receding to the right, large negative space on the right`.
- **Hero (banner largo, cerca de 1920x620, ~3,1:1):** `ultra-wide panoramic composition, the port at
  dusk, crane skyline, character small on the left, lots of horizontal space, no important detail in
  the centre-bottom`.
- **Logo (fundo transparente):** `title wordmark in a futuristic technical sans-serif, brushed steel
  letters with thin cyan edge-light, segmented circular HUD ring behind, transparent background`.
  Escrever o texto do nome do jogo na própria IA costuma sair torto: gere sem texto e componha a
  tipografia depois.
- **Ícone (quadrado, ex.: 256x256):** `square app-icon, close-up of the cyan segmented HUD ring
  with the silhouette of the operative inside, dark background, clean vector-like shapes`.

### 2.3 Direções alternativas (gere 2 ou 3 e escolha)
1. **Interior de poço circular:** vista de baixo para cima de um grande poço cilíndrico de metal
   com um círculo de céu azul no topo e uma silhueta pequena na borda (grande sensação de escala).
2. **Raios de luz da abertura:** um ponto central de onde saem dezenas de feixes verde-azulados
   finos sobre fundo preto com malha de circuitos (bom como fundo/hero abstrato, sem personagem).
3. **Plano de armas:** composição "inventário" com placas metálicas rebitadas de cinza-esverdeado e
   a silhueta de um fuzil em relevo, sob um halo ciano (bom para ícone/logo secundário).

### 2.4 Dimensões (a Steam; tamanhos de memória, não confirmados)
Capa 600x900, larga 920x430, hero 1920x620, logo com fundo transparente (cerca de 640 px de
largura), ícone quadrado. Gere maior (por exemplo 2048 px no lado maior) e recorte/reduza no
launcher. Mantenha áreas importantes longe das bordas.

## 3. Como o launcher usa o resultado
1. Coloque as imagens geradas em `<dados>/artwork/` com os nomes `capsule_vertical.png`,
   `capsule_wide.png`, `hero.png`, `logo.png`, `icon.png` (convenção a implementar no comando
   `artwork`).
2. O comando recorta/redimensiona para os tamanhos acima e instala em `grid/` com o appid certo
   (`steam-add`).
3. Se um arquivo faltar, o launcher recorta a partir da imagem vertical/larga disponível ou usa
   uma tela de carregamento do próprio dump (extraída localmente), nunca versionada.

## 4. Cuidados
- As imagens geradas ficam **fora do git** (regra 2) e são de uso pessoal. Se um dia o port for
  compartilhado, cada pessoa gera a própria arte.
- Peça às IAs **sem texto** e sem logotipos; não use os logos de estúdio vistos na abertura.
- Algumas ferramentas recusam nomes de franquias ou de personagens; a descrição acima evita
  depender disso. Se você quiser se aproximar do visual da série, descreva o estilo em vez de
  citar nomes.

## 5. Texturas guardadas para melhoria e upscaling (decisão do usuário)

As ~10,9 mil texturas convertidas ficam **guardadas localmente** em `work/art_all/` (`png/`,
`index.csv`, `sheets/`, `LEIA-ME.txt`), fora do git, como base para melhorar e fazer upscaling
depois. Observações para esse trabalho futuro:
- São texturas de PS2: a maioria em paleta (PSMT8/PSMT4), baixa resolução (64x64, 128x128, 128x64…)
  e com alfa PS2 já convertido para 0..255.
- Muitas são **repetidas por idioma ou por fase** (os mesmos caminhos aparecem em 5 cópias); vale
  deduplicar por hash antes de processar.
- Upscaling é caminho de mão dupla: para valer no jogo, o port precisaria de um **pacote de
  substituição de texturas** (carregado do disco local, nunca versionado nem distribuído) e de um
  backend de GPU que aceite texturas maiores que as originais. Isso se encaixa no plano de GPU
  (`../../docs/gs-gpu-plan.md`); a rasterização por software não é o lugar certo.
- Para a arte do atalho, use as imagens geradas (seção 2) ou as telas de carregamento; ambas são
  de uso pessoal.
- O arquivo `index.csv` e os scripts de conversão ficam como documentação de como foram geradas;
  o conversor usado está no scratchpad da sessão e deve virar `tools/zim_dump_all.py` quando for
  promovido (ferramenta genérica da raiz).
