# FreeRun LE + teste de parkour em NPC (passo 1, versão npc2)

Versão **de teste**, não para publicar. É o FreeRun LE 1.0.7 com um módulo novo: ao carregar **F10**, o NPC que estás a olhar tenta um movimento de parkour. Serve para responder a uma só pergunta: **o patch do FreeRun funciona num NPC?** Tudo o que acontece fica escrito no log.

> Versão npc2: corrige um crash ao carregar o save (variável do Havok sem alinhamento a 16 bytes; ver o Havok.cpp).
>
> Aviso: não consegui compilar nem correr o jogo. A sintaxe do módulo novo foi verificada com os cabeçalhos do projeto, mas o SKSE real ou o jogo podem mostrar erros. Se a compilação falhar, manda-me as linhas de erro do `build.log`.

## 1. Compilar
1. Cria um repositório novo no GitHub (por exemplo `FreeRunNPC-LE`).
2. Descompacta este ZIP e envia **tudo**: as pastas `.github` e `freerun-npc` e os ficheiros `LICENSE`, `LEIA-ME.md` e `NOTICE.md`.
3. Separador **Actions** → "Build FreeRun LE + teste NPC (passo 1)" → **Run workflow**.
4. No fim, descarrega o artefacto `resultado`: tem o `FreerunLE.dll` e o `build.log`.

## 2. Instalar
Precisas do FreeRun LE instalado normalmente, com o Nemesis já executado com "SkyParkour 1st Person" e "SkyParkour 3rd Person".

1. Faz uma cópia de `Data\SKSE\Plugins\FreerunLE.dll`.
2. Substitui-o pelo novo (o nome é igual de propósito, para nunca ficarem os dois).
3. Arranca o jogo. O log `Documentos\My Games\Skyrim\SKSE\FreerunLE.log` deve conter `Freerun LE 1.0.7-npc1` e `NPC test: on, key 68`.

## 3. Testar
1. Escolhe um NPC (um seguidor serve) e um obstáculo: caixote, muro baixo ou vedação.
2. Põe o NPC **encostado ao obstáculo** (a menos de uns 70 unidades, cerca de 1 m) e fica **atrás dele**, virado para o NPC e para o obstáculo.
3. Carrega **F10**. Aparece uma mensagem com o resultado.
4. Repete com obstáculos de alturas diferentes.

## 4. O que me mandar
- O ficheiro `FreerunLE.log`, logo a seguir ao teste.
- O que viste: não fez nada / animou mas ficou preso / subiu / caiu a ragdoll / o jogo fechou.

## 5. Opções (secção `[Npc]` em `Data\SKSE\Plugins\Freerun.ini`)
```
[Npc]
bEnabled=1
iTestKey=68
bAlignToPlayer=1
fMaxDistance=800
bMessages=1
```
`iTestKey=68` é a tecla F10 (código DirectInput). `bAlignToPlayer=1` vira o NPC para a direção do teu corpo antes de procurar a borda. `bMessages=0` desliga as caixas de mensagem (o log continua).

## 6. Voltar atrás
Repõe a cópia do `FreerunLE.dll` original.
