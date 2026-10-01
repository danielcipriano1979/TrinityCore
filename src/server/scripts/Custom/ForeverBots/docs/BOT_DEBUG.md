# ForeverBots — Debug, telemetria e testes

## Logs de decisão (BOT_DEBUG)

| Comando | Quem | Efeito |
|---|---|---|
| `.bot debug` / `.bot debug off` | jogador | liga ou desliga os logs dos bots do jogador |
| `.botdebug on` / `.botdebug off` | console/GM (`ra.ps1`) | todos os bots |

Os logs vão para `Server.log` (logger `server.foreverbots`). Cada bot loga quando a decisão muda, e a cada 3 s se ela não mudou:

```
[BotAI] Bot=Velria Role=heal State=COMBAT HP=88% Mana=64% Enemies=3 Decision=Flash Heal -> Gruk (Gruk at 31% (missing 812))
[BotAI] Bot=Gruk Role=tank State=COMBAT HP=31% Mana=0% Enemies=3 Decision=Taunt -> Ragefire Trogg (Ragefire Trogg is hitting Velria)
[BotAI] Bot=Xanmir Role=dps State=COMBAT HP=100% Mana=80% Enemies=3 Decision=HOLD_THREAT -> Ragefire Shaman (threat 93% of the tank)
```

Um bot que não alcança o alvo também loga: `[BotAI] Bot=X can't reach Y (dist 23.0), skipping it`.

## Telemetria

`.bot info` (jogador) ou `.bottest report` (console) mostra uma linha por bot com:
- papel e estado;
- vida e mana;
- DPS e HPS (sobre o tempo de combate) e overheal;
- mortes, abates, interrupts, dispels, taunts, CC e ressurreições;
- casts que deram certo e recusados;
- a decisão atual com o motivo.

Os contadores vêm dos hooks `DamageDealt` e `HealDone` (o overheal é calculado pela vida que faltava) e do resultado de cada cast.

## Teste sem client (grupo só de bots)

No servidor de teste (`custom\scripts\test-server.ps1`) com o console remoto (`custom\scripts\ra.ps1`):

```
.bottest party rfc 15        # tank warrior (líder), priest, mage, rogue, warlock nível 15 na entrada da Ragefire Chasm
.botdebug on
.bottest report              # telemetria
.bottest stop
```

`.bottest party tele <nome> [nível] [alliance]` usa um ponto do `game_tele`.

Como funciona: o tank é um bot "líder" sem dono. Os outros quatro são bots de grupo cujo dono é o líder (o mesmo caminho de login, grupo e relog dos bots de um jogador). O líder puxa e explora sozinho (`ProposePull`), o grupo segue o tank.

## `.botinfo`

Diagnóstico de baixo nível (atacantes, refs de combate, arma, autorepeat, alcance, facing) de todos os bots.
