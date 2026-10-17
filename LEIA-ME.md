# FreeRun LE + parkour em NPC (passo 3, versão npc8: o rasto do jogador)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail` e `NPC track`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## A ideia
Os logs mostraram que mandar a seguidora "procurar um obstáculo onde calha estar" só funciona às vezes, e as falhas coincidem com ela estar longe do ponto de arranque do movimento. O que **tu** fazes é melhor guia, porque já se sabe que funciona.

- **O rasto:** o plugin grava uma posição tua de 0,4 em 0,4 s (quando estás no chão e te mexeste pelo menos 40 unidades). Sempre que o FreeRun começa um movimento teu, grava também o movimento: tipo, ponto de arranque, direção e onde acabaste. Guarda os últimos 300 pontos e esquece tudo ao carregar um save ou ao viajar.
- **A seguidora:** se está seguida (F11), **pelo menos 120 unidades abaixo de ti**, a correr sem se aproximar (ou a correr no mesmo sítio), e há um ponto do teu rasto a menos de 350 unidades dela, ela **percorre o teu caminho** ponto a ponto, em corrida, e em cada ponto de movimento, em cima do ponto de arranque, **repete o mesmo movimento**. Pára quando está perto de ti e à tua altura.
- **O rasto tem de estar inteiro:** um salto simples ou uma queda deixam um buraco no rasto, e ela não copia saltos. Nesse caso volta ao método antigo (o obstáculo entre ela e ti), agora só com o ponto de arranque a menos de 30 unidades.
- **Durante a repetição** a física dela está desligada e é o plugin que a move, para a IA não a levar para outro lado. A animação de corrida dela continua. Se ficar bem ou mal só se vê no jogo.
- **Combate:** se ela tiver a arma na mão (ou sacar a arma a meio), o rasto não começa ou pára, e a luta fica com a IA.

Também mudou: o "puxão" até ao ponto de arranque passou a ser uma corrida curta (420 unidades por segundo), em vez de se esticar por 30 ticks.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc8`.

## 3. Testar
1. Carrega o save e olha para a seguidora com a câmara. Carrega **F11**.
2. Vai com ela para um sítio onde consigas subir com o FreeRun (uma rocha, um penedo, um muro alto). Sobe. No log vais ver linhas `NPC trail: recorded the player's climb ...`.
3. Fica em cima e espera. Quando ela começar a dar a volta e a afastar-se, deve aparecer `following his path from node ...` e ela passa a seguir o teu caminho.
4. Manda-me o `FreerunLE.log` e diz o que viste: se correu a direito, se repetiu bem os movimentos, se escorregou, se desapareceu, ficou dentro do chão.

O F10 continua a existir para testes manuais (o obstáculo entre ela e ti).

## 4. Opções (secção `[Npc]` em `Data\SKSE\Plugins\Freerun.ini`)
```
[Npc]
bEnabled=1
iTestKey=68           ; F10
iTrackKey=87          ; F11
bMessages=1
bAutoTracked=1        ; 0 = só registar (o rasto também fica desligado)
bTrail=1              ; 0 desliga o rasto
fTrailMinHeight=120   ; tu tens de estar pelo menos isto acima dela
fTrailJoinRadius=350  ; ela junta-se ao rasto num ponto a esta distância, no máximo
fTrailSpeed=330       ; velocidade ao longo do rasto (unidades por segundo)
fTrailEndRadius=220   ; chegou quando está a esta distância e à tua altura
fTrailMaxSec=45       ; duração máxima de uma repetição
fTrailSpacing=40      ; um ponto do rasto a cada tantas unidades
fDashSpeed=420        ; velocidade com que é levada ao ponto de arranque
fAutoMaxStartGap=30   ; método antigo: distância máxima ao ponto de arranque
fAutoCooldownSec=6.0
```
As outras opções do passo anterior (`fAutoMinDistance`, `fAutoDetourSec`, etc.) continuam válidas.

## 5. Ainda não faz
- Inimigos e NPCs em geral: hoje só segue quem apontas com F11. Descobrir quem é hostil é a fase seguinte.
- Copiar saltos simples.
- Mais de uma seguidora ao mesmo tempo.

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
