# FreeRun LE + parkour em NPC (passo 2a, versão npc4)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 com um módulo novo para NPCs. Tudo o que acontece fica escrito no log (`Documentos\My Games\Skyrim\SKSE\FreerunLE.log`).

> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto (a parte de exceções do Windows só pelo compilador real). Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## O que faz
- **F10, teste manual (como no passo 1):** o NPC que estás a olhar tenta o movimento de parkour que lhe calhar à frente. Aparece uma mensagem com o resultado.
- **F11, seguimento (novo):** o NPC que estás a olhar passa a ser "seguido". O estado dele vai para o log uma vez por segundo. Carrega F11 outra vez a olhar para ele para deixar de o seguir.
- **Modo automático (novo, ligado por defeito):** um NPC seguido que fica **parado**, **longe de ti**, **mais ou menos virado para ti** e com um **obstáculo à frente** que o FreeRun aceitaria, faz o movimento sozinho. Depois há uma pausa antes de poder repetir. Não aparece nenhuma caixa de mensagem nos movimentos automáticos.

Só **um NPC de cada vez** é seguido. Depois de carregar um save, volta a carregar F11 a olhar para ela (o jogo não guarda quem estava a ser seguido).

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
1. Carrega o save e anda um pouco **sem carregar em nada**, para confirmar que não há crash nem ícone a piscar.
2. Olha para a seguidora e carrega **F11**. Deve aparecer a mensagem "tracking this NPC".
3. Afasta-te dela de forma a que ela fique **presa num obstáculo** (muro baixo, caixote, vedação) entre vocês os dois, a mais de uns 250 de distância. Espera uns 2 segundos.
4. Se tudo estiver certo, ela tenta o movimento sozinha. Se não, o log diz porquê: procura as linhas `NPC track`.
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
fAutoMinDistance=250  ; só atua se estiver pelo menos a esta distância de ti
fAutoMaxDistance=1500 ; ... e no máximo a esta
fAutoStuckSec=2.0     ; tempo parada antes de atuar
fAutoMaxMove=40       ; mexer menos do que isto conta como estar parada
fAutoCooldownSec=6.0  ; pausa entre dois movimentos automáticos
```
Se mandares a seguidora **esperar** num sítio, ela fica parada: carrega F11 a olhar para ela para deixar de a seguir, senão pode trepar o que estiver à frente dela.

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
