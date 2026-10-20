# FreeRun LE + parkour em NPC (passo 4, versão npc13: várias seguidoras e resgate)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail`, `NPC track` e `NPC landing`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc13 (a partir do log da npc12)
O log da npc12: 20 movimentos repetidos, nenhum falhou, sem exceções. O problema está no que vem **depois**: a seguidora fica parada (velocidade 0,00) enquanto tu te afastas, uma vez durante **98 s e a 3500 unidades**. Isso não é a física nem o rumo: é a IA dela que deixou de lhe pedir para andar. A razão mais provável é a mais simples: **o topo de uma pedra não está na malha de navegação**, por isso não há caminho de lá até ti. Para além disso:
- **Várias seguidoras:** agora podes seguir **até 3 NPCs** com F11 (carregar outra vez a olhar para um deles pára de o seguir). Só uma faz o movimento de cada vez: as outras esperam a sua vez (a resposta dura 10 s), por isso vão uma a seguir à outra.
- **Resgate de uma seguidora parada:** se uma seguidora seguida está parada há 15 s, a mais de 450 unidades, com a velocidade a 0 (a IA não lhe pede para andar), é **posta de volta no teu rasto, um pouco atrás de ti**: um sítio onde tu já estiveste, portanto onde a IA já pode traçar um caminho. É o último recurso; `fStrandedSec=0` desliga-o.
- **Movimentos interrompidos não são repetidos:** num movimento teu que parou ao fim de 375 ms (caiste ou cancelaste), ela repetia a animação toda, retida por um caminho de poucos ticks. Agora, abaixo de 900 ms (`iMinMoveMs`) o movimento é ignorado.
- **Se alguma coisa der exceção** dentro do módulo, a seguidora que estava a ser movida recebe a física de volta (antes ficava com ela desligada).

## O que mudou na npc12 (resumo)
O corpo dela é levado ao longo do caminho que o teu corpo fez (grava-se tick a tick), com a física desligada reafirmada a cada tick, e a verificação do caminho segue o chão (`bPathReplay=1`).

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc13`.

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
fStrandedSec=15       ; parada tanto tempo, longe de ti, a IA sem lhe pedir para andar: é posta no teu rasto (0 = nunca)
fStrandedMinDistance=450
iMinMoveMs=900        ; um movimento teu que parou antes disto foi interrompido e não é repetido
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
