# FreeRun LE + parkour em NPC (passo 4, versão npc14: modelos de caminho e experiência anim-driven)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail`, `NPC track` e `NPC landing`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc14 (a caminho da autonomia)
Dois passos pequenos, os que não dependem de saber mais sobre o motor:
1. **Um caminho por tipo de movimento.** O último caminho do teu corpo em cada tipo de movimento (vault, escalada baixa, média, alta, altíssima) fica guardado como modelo. Quando uma NPC decide sozinha que há algo para trepar (o gatilho antigo), já não fica entregue ao motor, que é onde estava a luta: leva o modelo desse tipo, **rodado para o rumo com que ela faz o movimento**. A animação move-se da mesma maneira em qualquer sítio. Os modelos duram a sessão: precisas de ter feito cada tipo uma vez. No log: `kept as the template for a ...` e `carried along a template path ...`.
2. **Experiência `bAnimDriven=1`** (desligada por defeito). Diz ao motor, durante o movimento, que o corpo dela é guiado pela animação (a variável `bAnimationDriven` do grafo, a mesma que o jogo põe nas animações sincronizadas e nas animações de cena). Se o motor a respeitar, é **o próprio motor** que põe de lado a IA e a física dela, e não há nada a combater. Para a testar com limpeza: `bPathReplay=0` e `bAnimDriven=1`, e compara com `bAnimDriven=0` (a diferença deve ver-se em encostas: se ela deixa de deslizar, funciona).

## O que mudou na npc13
- **Várias seguidoras:** até 3 NPCs com F11, uma de cada vez a fazer o movimento (as outras esperam, a resposta dura 10 s).
- **Resgate:** parada há 15 s, a mais de 450 unidades, velocidade 0: é posta no teu rasto, um pouco atrás de ti (`fStrandedSec`, 0 desliga). O log mostrou que depois disso a IA volta a mandá-la correr.
- **Movimentos interrompidos** (`iMinMoveMs=900`) não são repetidos. Se houver exceção, a seguidora recebe a física de volta.

## O que mudou na npc12 (resumo)
O corpo dela é levado ao longo do caminho que o teu corpo fez (grava-se tick a tick), com a física desligada reafirmada a cada tick, e a verificação do caminho segue o chão (`bPathReplay=1`).

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc14`.

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
bAnimDriven=0         ; experiência: dizer ao motor que o corpo dela é guiado pela animação durante o movimento
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
