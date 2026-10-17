# FreeRun LE + parkour em NPC (passo 3, versão npc9: o rasto, só movimentos)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail` e `NPC track`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc9
O log da npc8 mostrou o que corria mal:
- **Um bug meu na correção de posição:** o "empurrão" até ao ponto de arranque era medido de novo a cada tick, em vez de uma só vez. Quando a animação a levava para longe do ponto (a subir), a correção arrastava-a **de volta**. Das 13 repetições, **7 acabaram sem subir nada** (algumas a descer), mesmo com a animação a tocar. Agora o desvio é medido **uma vez** e percorrido na sua direção original, como o FreeRun faz com o jogador.
- **Andar o rasto todo causava os outros problemas:** ela era movida em linha reta, com a física desligada, durante vários segundos: atravessava o terreno (por baixo do chão), deslizava com a animação a não acompanhar, e virava a cada tick (corpo inclinado). Por defeito, **ela já não anda o rasto**.

## Como funciona agora
- **O rasto:** o plugin grava as tuas posições e, sempre que o FreeRun começa um movimento teu, o movimento (tipo, ponto de arranque, direção, onde acabaste).
- **Seguidora seguida (F11):** quando estás pelo menos 120 unidades acima dela e ela não se aproxima (ou corre no mesmo sítio), o plugin procura um **movimento teu cujo ponto de arranque esteja a menos de 120 unidades dela**, que a levasse pelo menos 40 unidades mais acima, e com o caminho até ele livre (uma esfera é varrida de um ponto ao outro). Se houver, ela corre esse pequeno trecho até ao ponto de arranque, vira-se como tu estavas virado e **repete o movimento**. Depois a IA leva-a daí. Pausa de 2 s antes de poder repetir outro, para encadear subidas.
- **Quem a leva ao pé da parede é a IA**, que a está a mandar para onde tu estás.
- **Combate:** com a arma na mão, não começa; se sacar a arma a meio, pára.
- **Modo de caminhar o rasto** (`bTrailWalk=1`, experimental): mantém-se, agora com viragem suave e ela é colocada no chão a cada passo, mas é o que dava mais problemas.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc9`.

## 3. Testar
1. Carrega o save e olha para a seguidora com a câmara. Carrega **F11**.
2. Vai com ela para um sítio onde consigas subir com o FreeRun (uma rocha, um penedo, um muro alto). Sobe. No log vais ver linhas `NPC trail: recorded the player's climb ...`.
3. Fica em cima e espera. Quando ela começar a dar a volta, deve aparecer `starts ... units from her: going to its start mark to repeat it` e ela repete a tua subida a partir do pé do penedo.
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
fTrailJoinRadius=350  ; só no modo de caminhar
bTrailWalk=0          ; 1 = também anda o rasto todo (experimental)
fTrailMoveRadius=120  ; repete um movimento teu se estiver a esta distância do ponto de arranque
fTrailSpeed=330       ; só no modo de caminhar
fTrailEndRadius=220   ; só no modo de caminhar
fTrailMaxSec=45       ; duração máxima
fTrailSpacing=40      ; um ponto do rasto a cada tantas unidades
fDashSpeed=420        ; velocidade com que é levada ao ponto de arranque
fAutoMaxStartGap=30   ; método antigo: distância máxima ao ponto de arranque
fAutoCooldownSec=6.0
```
As outras opções do passo anterior (`fAutoMinDistance`, `fAutoDetourSec`, etc.) continuam válidas.

## 5. Ainda não faz
- Inimigos e NPCs em geral: hoje só segue quem apontas com F11. Descobrir quem é hostil é a fase seguinte.
- Copiar saltos simples.
- Usar as animações nativas do Fluid Motion LITE em vez das do FreeRun.
- Mais de uma seguidora ao mesmo tempo.

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
