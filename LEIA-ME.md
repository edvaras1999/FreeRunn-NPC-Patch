# FreeRun LE + parkour em NPC (passo 2b, versão npc7)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 (código de 2 de outubro) com um módulo novo para NPCs. Tudo o que acontece fica no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`).

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

> **Versão npc7, o que mudou**
> - **Inventário preso e ícone a oscilar:** o código de 2 de outubro lia a chave `bShowIndicators` (por defeito **ligada**), mas o teu `Freerun.ini` usa `bShowLedgeMarker=0`, a chave da versão publicada. Por isso o marcador nunca ficava desligado: o menu "Freerun" abria com o HUD, ficava sempre aberto e prendia o inventário. Agora lê `bShowLedgeMarker`, **desligado por defeito**, e o menu só abre se o ligares.
> - **O automático falhava depois de arrancar:** o plugin decidia o movimento, virava-a, esperava 0,4 s e procurava o obstáculo outra vez, mas ela já tinha corrido para longe. Agora a procura é feita uma só vez, ela fica **parada no sítio** (física desligada) enquanto vira, e o movimento arranca ao fim de 0,15 s com o obstáculo já conhecido.
> - **"Animação guiada" já não impede o automático:** nos logs, esse estado aparecia exatamente quando ela estava a bater contra o obstáculo.
> - **F10 mais fácil:** em vez de exigir que ela esteja virada para o obstáculo, tenta o obstáculo **entre ela e ti**, a partir de onde ela está.

## O que faz
- **F10, teste manual:** o NPC que estás a olhar tenta subir o obstáculo que está entre ela e ti. Basta ela estar ao pé do obstáculo (até 150 unidades do ponto de arranque) e tu do outro lado. Aparece uma mensagem com o resultado.
- **F11, seguimento:** o NPC que estás a olhar passa a ser "seguido" (pontaria pela **câmara** em terceira pessoa). O estado dele vai para o log uma vez por segundo. Carrega F11 outra vez a olhar para ele para parar. O seguimento é esquecido ao carregar um save.
- **Automático (ligado por defeito):** enquanto a seguidora está em **DETOUR** (a correr sem se aproximar de ti nos últimos 3 s) ou **PUSHING** (a correr no mesmo sítio), o plugin procura duas vezes por segundo um obstáculo escalável **na direção de ti**. Se o ponto de arranque estiver a menos de 90 unidades dela e subir não a afastar da tua altura, ela vira-se para ti e faz o movimento. Depois há uma pausa de 6 s.

## 1. Compilar
1. Atualiza o repositório do GitHub com **tudo** o que está neste ZIP (pastas `.github` e `freerun-npc` e os ficheiros da raiz).
2. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
3. Descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Substitui `Data\SKSE\Plugins\FreerunLE.dll` (guarda uma cópia). O log deve dizer `Freerun LE 1.0.7-npc7`. O teu `Freerun.ini` serve como está; as opções novas são opcionais.

## 3. Testar
1. Abre o inventário algumas vezes: tem de abrir, responder e fechar.
2. Olha para a seguidora com a câmara e carrega **F11**.
3. Vai para um sítio mais alto (uma rocha ou penedo que o FreeRun consiga subir) e deixa-a tentar chegar a ti. As montanhas altas continuam a ser pouco prováveis (precisam de uma aresta junto ao chão); rochas e muros de 1 a 3 m são o caso certo.
4. Para forçar um teste: põe-na ao pé de um muro baixo, do lado contrário ao teu, olha para ela e carrega **F10**.
5. Manda-me o `FreerunLE.log` e o que viste.

## 4. Opções (secção `[Npc]` em `Data\SKSE\Plugins\Freerun.ini`)
```
[Npc]
bEnabled=1
iTestKey=68           ; F10, teste manual
iTrackKey=87          ; F11, seguir / deixar de seguir
bMessages=1           ; 0 desliga as caixas de mensagem (o log continua)
fMaxDistance=800      ; alcance da pontaria
bManualTowardPlayer=1 ; F10 tenta o obstáculo entre ela e ti
fManualMaxStartGap=150
bAutoTracked=1        ; 0 = só registar, nunca mexer sozinha
fAutoMinDistance=200
fAutoMaxDistance=1500
fAutoDetourSec=3.0
fAutoMinApproach=30
fAutoStuckSec=2.0
fAutoMaxMove=40
fAutoMaxStartGap=90
fAutoCooldownSec=6.0
```
Se mandares a seguidora **esperar**, carrega F11 a olhar para ela para deixar de a seguir.

## 5. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
