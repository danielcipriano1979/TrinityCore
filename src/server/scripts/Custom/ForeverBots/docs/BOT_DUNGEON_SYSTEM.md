# ForeverBots — Dungeons

## O que existe

- **Instância:** os bots de grupo entram na instância do dono por relog (`UpdatePartyBot`: o dono mudou de mapa ou de instância). Membros do grupo caem na mesma instância.
- **Estado dos chefes:** os scripts de instância expõem o estado de cada chefe (`InstanceScript::GetBossState`). A Ragefire Chasm tem script Classic com os 4 chefes, e o estado é gravado pela SmartAI deles (`[RFC] … engaged / reset / defeated`).
- **Grupo só de bots (`.bottest party`):** o tank lidera.
  - **Prontidão:** ninguém morto, todos a até 35 jardas, vida ≥ 70%, mana do healer ≥ 60%, ninguém lançando magia. O motivo da espera aparece no log (`WAIT_PARTY … (Velria at 54% mana)`).
  - **Pull:** o mob hostil mais próximo **pelo caminho** (`PathGenerator`, onde há mmaps) até 45 jardas. Inalcançáveis são ignorados.
  - **Exploração:** sem nada a 45 jardas, anda até 25 jardas do próximo mob conhecido num raio de 150 jardas.
  - **Seguidores:** seguem o tank em formação. Um follow travado vira caminho direto e, depois de 12 s, reagrupamento fora de combate.
- **Mapas de caminho:** mmaps da Ragefire Chasm (389) gerados. Nos outros mapas o movimento ainda é em linha reta.

## O que falta (fases 6–8)

| Item | Plano |
|---|---|
| Rota da dungeon | Pontos de rota por instância (tabela), para não depender só do "mais próximo"; evita puxar o fundo antes da entrada |
| Patrulhas e packs | Agrupar mobs por `creature_formations`/proximidade e puxar o pack inteiro de uma vez; esperar patrulhas |
| Chefes | Ler `GetBossState` para não puxar chefe com o grupo sem mana cheia; posicionar o chefe de costas para o grupo |
| Portas, eventos, objetos | Interagir com `GameObject` exigido pelo script da instância |
| Wipe | Detectar wipe (todos mortos), ressuscitar no corpo, voltar ao último ponto seguro, resetar o encontro (o InstanceScript já reseta o chefe com o evade) |
| Loot | Roll Need/Greed por upgrade; precisa de acessor dos rolls pendentes no core |
| Outros mapas | Gerar mmaps de mais dungeons (um mapa por vez, `mmaps_generator <id>`) |
