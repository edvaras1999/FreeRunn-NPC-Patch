# FreeRun LE + parkour em NPC (passo 4, versão npc11: rumo corrigido e assentamento suave)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`), nas linhas `NPC trail`, `NPC track` e `NPC landing`.

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que mudou na npc11 (a partir do log da npc10)
O log mostrou três causas para as falhas estranhas:
1. **O rumo do corpo dela não acompanhava a rotação.** Com a física desligada, o controlador de personagem não segue uma rotação do ator; o "rumo" dele ficou parado durante o movimento todo. Em 14 das 17 repetições o erro de rumo no arranque foi de **44° a 175°**, e a animação move-a ao longo desse rumo: é o "escalar no ar, longe do local certo". Os piores resultados (subida negativa, deslocamento de 160 a 240 unidades) coincidem com os maiores erros. Agora o rumo do controlador é escrito diretamente, ela é **fixada no ponto de arranque, virada para o lado certo, a cada tick**, e o movimento só é enviado quando o rumo bate certo (ou ao fim de 0,6 s).
2. **A ida a pé até ao ponto andava às voltas.** Cada passo partia de onde o motor a tinha deixado, e a IA dela continua a mexê-la, por isso o caminho virava um emaranhado (corpo a girar, deslizar, e duas vezes 25 s sem chegar). Agora o passo parte de uma posição **dela própria**, escrita todos os ticks. A chegada conta-se só na distância plana, o rumo só muda enquanto ainda falta caminho e a velocidades limitadas, e há um **prazo** para chegar (distância a dividir pela velocidade, mais 4 s).
3. **O assentamento era um salto instantâneo** (até 62 unidades, uma vez 116). Agora é um **deslize suavizado de 0,28 s** (`iLandingGlideMs`), o limite caiu de 140 para 80 unidades, e o resultado dela só é avaliado quando o deslize acaba.

Sobre o **passo com animação** que sugeriste: não é possível ainda. Para o corpo dar um passo "a sério" seria preciso guiar o grafo de locomoção (as variáveis de velocidade e direção) e não conheço uma forma de o fazer com o que o FreeRun já expõe. Fica o deslize suavizado como solução provisória, e a investigação desse passo como próximo ponto.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP.
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc11`.

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
