# FreeRun LE + parkour em NPC (passo 2b, versão npc6)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 com um módulo novo para NPCs. Tudo o que acontece fica escrito no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`).

> **Versão npc6, o que mudou:**
> - **Crash ao deslizar (slide/dodge):** o `Engine.h` do FreeRun declarava `PlayerIsInMidair` (endereço `0x006AB2F0`) como função normal, mas é uma função-membro que recebe o jogador em ECX. Chamada assim, o motor lê um ECX qualquer, que dependia do compilador. O `Engine.h` e o `Engine.cpp` agora são os do teu FreeClimb LE, que já tem isto corrigido e valida a cadeia de ponteiros do jogador.
> - **Pontaria do F10/F11:** em terceira pessoa passa a seguir a **câmara**, não o corpo.
> - **O módulo NPC corre dentro de um bloco protegido contra exceções**, porque é chamado a partir de uma thread de trabalho da IA, e a seguidora seguida é esquecida ao carregar um save (um ponteiro guardado de um mundo antigo é um ponteiro para memória libertada).
>
> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto (a parte de exceções do Windows só pelo compilador real). Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que faz
- **F10, teste manual (como no passo 1):** o NPC que estás a olhar tenta o movimento de parkour que lhe calhar à frente.
- **F11, seguimento:** o NPC que estás a olhar passa a ser "seguido". O estado dele vai para o log uma vez por segundo. Carrega F11 outra vez a olhar para ele para deixar de o seguir.
- **Modo automático (ligado por defeito), mudou nesta versão.** O primeiro log mostrou que a seguidora não fica parada junto ao obstáculo: o pathing manda-a dar a volta, e ela corre, a afastar-se de ti, durante vários segundos. Agora o plugin deteta duas situações:
  - **DETOUR:** ela corre (velocidade de animação ≥ 0,5) e **não se aproximou** de ti nos últimos 3 s.
  - **PUSHING:** ela está a correr no mesmo sítio (parada, com a velocidade de animação alta).

  Enquanto uma delas se verifica, duas vezes por segundo o plugin faz duas procuras de obstáculo: uma para onde ela está virada e outra **em linha reta para ti**. Se houver um obstáculo escalável logo ali (com o ponto de arranque a menos de 90 unidades dela) e subir não a afastar da tua altura, ela vira-se para ti e faz o movimento. Depois há uma pausa de 6 s.

Só **um NPC de cada vez** é seguido. Depois de carregar um save, volta a carregar F11 a olhar para ela.

## 1. Compilar
1. Cria um repositório novo no GitHub (ou atualiza o do passo 1).
2. Descompacta este ZIP e envia **tudo**: as pastas `.github` e `freerun-npc` e os ficheiros `LICENSE`, `LEIA-ME.md` e `NOTICE.md`.
3. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
4. No fim, descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Precisas do FreeRun LE instalado normalmente (Nemesis já executado com os patches 1st e 3rd Person).

1. Faz uma cópia de `Data\SKSE\Plugins\FreerunLE.dll`.
2. Substitui-o pelo novo. O log deve dizer `Freerun LE 1.0.7-npc4`.
3. Se já tens a secção `[Npc]` no `Freerun.ini`, podes deixá-la como está. As opções novas são opcionais (ver abaixo).

## 3. Testar
Os movimentos automáticos não mostram caixas de mensagem: o que aconteceu vê-se no log.


1. Carrega o save e anda um pouco **sem carregar em nada**, para confirmar que não há crash nem ícone a piscar.
2. Olha para a seguidora e carrega **F11**. Deve aparecer a mensagem "tracking this NPC".
3. Põe um obstáculo escalável (muro baixo, caixote, vedação) **entre vocês os dois**, com o caminho "normal" a obrigá-la a dar a volta, e afasta-te. Deixa-a a correr por ele.
4. Se ela subir, ótimo. Se não, o log diz porquê: procura as linhas `NPC track`. Linhas com **DETOUR** ou **PUSHING** mostram quando o plugin achou que ela estava em apuros, e as linhas `a ledge ...` dizem o que a procura encontrou e porque foi (ou não) aceite.
5. O F10 continua a funcionar como antes para testes manuais.

Para só **registar** sem ela se mexer sozinha, põe `bAutoTracked=0`.

## 4. O que me mandar
- O `FreerunLE.log`, logo a seguir ao teste.
- O que viste: não fez nada / fez o movimento certo / fez sem estar presa / ficou presa / caiu a ragdoll / o jogo fechou.

## 5. Opções (secção `[Npc]` em `Data\SKSE\Plugins\Freerun.ini`)
```
[Npc]
bEnabled=1
iTestKey=68           ; F10, teste manual
iTrackKey=87          ; F11, seguir / deixar de seguir
bAlignToPlayer=1
fMaxDistance=800      ; alcance da pontaria
bMessages=1           ; 0 desliga as caixas de mensagem (o log continua)
bAutoTracked=1        ; 0 = só registar, nunca mexer sozinha
fAutoMinDistance=200  ; só atua se estiver pelo menos a esta distância de ti
fAutoMaxDistance=1500 ; ... e no máximo a esta
fAutoDetourSec=3.0    ; janela para medir se ela se aproximou (1 a 3,5)
fAutoMinApproach=30   ; aproximar-se menos do que isto na janela = desvio
fAutoStuckSec=2.0     ; PUSHING: tempo parada com a animação a correr
fAutoMaxMove=40       ; mexer menos do que isto conta como estar parada
fAutoMaxStartGap=90   ; o ponto de arranque do movimento tem de estar a esta distância dela, no máximo
fAutoCooldownSec=6.0  ; pausa entre dois movimentos automáticos
```
Se mandares a seguidora **esperar** num sítio, ela fica parada: carrega F11 a olhar para ela para deixar de a seguir, senão pode trepar o que estiver à frente dela.

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
