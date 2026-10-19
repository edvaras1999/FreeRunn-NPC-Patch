# FreeRun LE + parkour em NPC (passo 4, versão npc12: o caminho do teu corpo)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail`, `NPC track` e `NPC landing`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc12 (a partir do log da npc11)
O que o log da npc11 mostrou:
- **O rumo ficou resolvido:** em 23 repetições o erro de rumo foi de 0° a 6° (antes, 44° a 175°).
- **19 de 23 repetições acabaram em cima** (de +96 a +198). As 4 más foram **a descer e de lado**: em "climb low" com a animação a correr, o corpo dela foi para baixo a um ritmo constante (cerca de 75 unidades por segundo, 100 unidades em 1,4 s) e 76 unidades para o lado, com o estado do controlador "no chão". Ela tinha começado o movimento numa encosta, e foi escorregando por ela: **a física dela estava a atuar, apesar de eu a ter desligado**. É a "luta com a IA" que notaste: o motor (a IA dela e a física) continua a mexer-lhe no corpo, e o que eu faço por cima é desfeito.
- **Quando não responde:** 77 vezes. 46 por "algo sólido entre ela e o sítio" (a minha verificação do caminho varria um plano horizontal, que bate no chão a subir ao pé de qualquer encosta) e 31 por estar longe.

Por isso:
1. **O corpo dela é levado ao longo do caminho que o teu corpo fez.** Durante cada movimento teu, o plugin grava, tick a tick, onde o teu corpo está, relativamente ao ponto de arranque, e tira a parte do "puxão" ao ponto (ela já começa no ponto). Quando ela repete o movimento, o plugin **escreve a posição dela em cada tick** ao longo desse caminho, com a animação dela a tocar. O motor deixa de ser quem a move, e não há nada com que a IA ou a física possam lutar. Isto vale para os movimentos que ela repete a partir do rasto (a resposta aos teus movimentos). Podes desligar com `bPathReplay=0`.
2. O **NoSim é reafirmado a cada tick** enquanto ela está a ser levada.
3. **A verificação do caminho** passou a seguir o chão: troços de até 120 unidades, cada um à altura da cintura **por cima do chão desse sítio**, e o chão onde se pode andar já não conta como obstáculo.

## Como funciona (resumo)
- **O rasto:** o plugin grava as tuas posições e, sempre que o FreeRun começa um movimento teu, o movimento (tipo, ponto de arranque, direção, caminho do corpo, onde acabaste).
- **Seguidora seguida (F11):** quando fazes um movimento, a que está atrás de ti, a menos de 500 unidades e com o caminho livre, **vai ao mesmo sítio, a pé, e faz o mesmo**, levada ao longo do teu caminho. Se não acabar em cima, volta a tentar (até 3 vezes). O fim de cada escalada é um deslize suave para cima da superfície (`iLandingGlideMs`).
- **Combate:** com a arma na mão, não começa; se sacar a arma a meio, pára.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc12`.

## 3. Testar
1. Olha para a seguidora com a câmara e carrega **F11**.
2. Com ela **atrás de ti, a 2 ou 3 metros**, salta uma cerca com o FreeRun (vault) ou sobe um muro. Deve aparecer `the player made a ... she goes to the same spot and does it too`, e ela faz o mesmo no mesmo sítio.
3. Sobe uma escalada alta e vê se ficas em cima, um passo para lá do bordo, sem escorregar (linha `NPC landing: moved ... units`).
4. Manda-me o `FreerunLE.log` e diz o que viste.

## 4. Opções (secção `[Npc]` em `Data\SKSE\Plugins\Freerun.ini`)
```
[Npc]
bTrailCall=1          ; 0 desliga a resposta aos teus movimentos
fCallRadius=500       ; ela responde a um movimento teu feito a esta distância dela, no máximo
fCallWindowSec=10     ; ... durante este tempo
iCallAttempts=3       ; tentativas se não acabar em cima
bLandingAssist=1      ; 0 desliga o assentamento no topo (nela e em ti)
fLandingForward=38    ; quanto passa da aresta
iLandingGlideMs=280   ; duração do deslize até lá (0 = instantâneo)
bPathReplay=1         ; ela é levada ao longo do caminho que o teu corpo fez (0 = deixar ao motor)
bTrail=1
bTrailWalk=0          ; 1 = também anda o rasto todo (experimental, deu problemas)
fTrailMinHeight=120   ; gatilho antigo: tu tens de estar pelo menos isto acima dela
fTrailMoveRadius=120  ; gatilho antigo: distância ao ponto de arranque
fTrailSpeed=330       ; velocidade em que ela vai a pé até ao ponto de arranque (unidades por segundo)
fDashSpeed=420
fAutoCooldownSec=6.0
```
As outras opções do passo anterior continuam válidas.

## 5. Ainda não faz
- Inimigos e NPCs em geral: hoje só segue quem apontas com F11.
- Copiar saltos simples.
- Mais de uma seguidora ao mesmo tempo.
- Usar as animações nativas do Fluid Motion LITE em vez das do FreeRun.

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
