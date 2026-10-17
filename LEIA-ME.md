# FreeRun LE + parkour em NPC (passo 4, versão npc10: ela responde aos teus movimentos)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail`, `NPC track` e `NPC landing`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc10
1. **O gatilho é o teu movimento do FreeRun** (e não a altura). Quando começas um movimento (vault, escalada), a seguidora seguida (F11) que está **atrás de ti** e a **menos de 500 unidades** do sítio onde o fizeste, com o caminho livre até esse ponto, **vai a esse mesmo sítio e faz o mesmo movimento**. Responde até 10 s depois de o teres feito, e responde aos teus movimentos pela ordem em que os fizeste. Se ela já passou o ponto (está do outro lado) ou já está tão alta como a escalada acabou, o movimento conta como respondido. Degraus pequenos não são copiados (ela sobe-os sozinha).
2. **Assentamento no topo.** No fim de uma escalada (o teu e o dela), o corpo é pousado **em cima da superfície, um passo (38 unidades) para lá da aresta**. A animação acaba mesmo no bordo, e é aí que o corpo escorrega. O ponto é verificado antes: tem de haver chão à altura da aresta, o caminho até lá tem de estar livre, e não pode ficar longe de onde o corpo já está. Se não houver sítio seguro, fica onde a animação acabou. Isto aplica-se também ao **jogador** (no fim de uma escalada do FreeRun), e pode desligar-se (`bLandingAssist=0`).
3. **Insistência.** Se ela não acabar em cima (abaixo da altura da aresta), **volta ao ponto de arranque e tenta outra vez**, até 3 tentativas, sem a largar à IA, que a mandaria pelo caminho longo.

O gatilho antigo (ela a correr sem se aproximar enquanto estás bem mais alto) mantém-se como segunda opção.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc10`.

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
