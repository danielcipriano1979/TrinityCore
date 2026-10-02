# ForeverBots — Status de implementação

Atualizado em 2026-10-01. Legenda: **[OK]** implementado e validado no servidor de teste · **[PARTIAL]** existe, com lacunas conhecidas · **[TODO]** pendente · **[BLOCKED]** depende de outra coisa.

"Validado" significa testado com um grupo só de bots (`.bottest party rfc 15`) no servidor de teste, **sem client**. Nada desta fase foi visto ainda num client real.

## Resultado dos testes (Ragefire Chasm, grupo nível 15: warrior tank, priest, mage, rogue, warlock)

| Rodada | Duração | Abates | Mortes | Chefes | Observação |
|---|---|---|---|---|---|
| Melhor | 14 min | 110+ | 0 | Taragaman, Jergosh, Bazzalan mortos | O tank parou sem alvo depois (sem busca no mapa inteiro) |
| Última | 18 min | ~50 | 0 | Oggleflint engajado e resetado | O grupo não alcançou o chefe; o tank ficou sem alvo alcançável |

Medido em todas as rodadas:
- **Recusas de cast:** o tank caiu de 221 para 0 a 2 por rodada.
- **Interrupts:** de 7 a 13 por rodada, com reserva funcionando (Kick, Shield Bash).
- **Controle:** Polymorph e Frost Nova em packs.
- **Nenhum bot morreu.**

O conteúdo foi fácil demais para testar a cura: o tank quase não perdeu vida, então o healer quase não curou.

## Sistemas

| Sistema | Status | Notas |
|---|---|---|
| BotController (`ForeverBotAI`) | [OK] | Tick de 250–400 ms por bot (variação) + tick lento de 2 s |
| BotBrain (relevância) | [OK] | Candidato que falha não gasta o tick |
| BotSpellBook (classificação SpellInfo) | [OK] | Só magias Classic (treinador, talento, quest, nível 1) |
| PartyBlackboard | [OK] | Reservas de interrupt/CC, curas em andamento, foco, pull, engajamento |
| TargetAI | [OK] | Pontuação: foco, alvo do tank, peel do healer, CC −500, vida, distância |
| ThreatAI | [PARTIAL] | DPS segura com 90%; redução de ameaça. Não usa threat de pets |
| Tank | [OK] | Mob solto, taunt (simples e em área), ameaça em área, defensivos |
| Healer | [PARTIAL] | Pontuação, eficiência, escudo/HoT/área, mana. Pouco testado (sem dano no teste) |
| DPS | [OK] | Abertura do tank, threat hold, rotação de classe + genérica |
| Interrupt | [OK] | Reserva no blackboard, validado |
| Dispel | [PARTIAL] | Implementado; poucos debuffs no teste |
| Crowd control | [PARTIAL] | Polymorph/Sap/Banish com reserva; fear desligado em grupo |
| Defensivos | [PARTIAL] | Pelo spellbook; não usa poções |
| Buffs | [OK] | Próprios, de grupo, exclusivos pela classe; situacionais excluídos |
| Ressurreição | [PARTIAL] | Healer ressuscita corpos; bot aceita pedido. Sem morte no teste para validar |
| MovementAI | [PARTIAL] | Posição por papel, LOS do servidor, stuck, follow travado → reagrupar. Sem mmaps fora da RFC |
| PullManager (grupo só de bots) | [PARTIAL] | Prontidão, pull pelo caminho, exploração; trava quando nada é alcançável |
| DungeonAI (rotas, packs, portas) | [TODO] | Próximo passo: rotas da RFC |
| MechanicAI (AoE no chão, cones) | [TODO] | `BOT_MECHANICS.md` |
| BossAI (estado via InstanceScript) | [PARTIAL] | Estado lido/gravado (RFC); sem uso tático ainda |
| Wipe / recovery | [PARTIAL] | Ressurreição após wipe pelo manager; sem voltar ao ponto seguro |
| LootManager | [TODO] | Precisa de acessor de rolls no core |
| Telemetria | [OK] | DPS, HPS, overheal, mortes, interrupts, dispels, taunts, CC, res, casts |
| Debug | [OK] | `.bot debug`, `.botdebug`, logs `[BotAI]` com tempo, motivos de recusa e de espera |
| Personalidade / tempo de reação | [PARTIAL] | Só a variação do tick |
| Configuração | [PARTIAL] | `ForeverBots.Debug`, `ForeverBots.WorldBots`; o resto é constante no código |
| Performance | [PARTIAL] | Contexto por tick, caminhos limitados a 8 por decisão, inalcançáveis em cache |

## Classes (perfil + spellbook)

| Classe | Tank | Healer | DPS | Notas |
|---|---|---|---|---|
| Warrior | [OK] | — | [PARTIAL] | Defensive Stance por quest ensinada aos bots |
| Paladin | [PARTIAL] | [PARTIAL] | [PARTIAL] | Não testado |
| Hunter | — | — | [PARTIAL] | Sem pet (bots não têm pet domado) |
| Rogue | — | — | [OK] | Validado (Kick, Sinister Strike, Eviscerate, Slice and Dice) |
| Priest | — | [PARTIAL] | [PARTIAL] | Cura pouco exercitada |
| Shaman | — | [PARTIAL] | [PARTIAL] | Totens e imbuições; não testado |
| Mage | — | — | [OK] | Validado (Arcane Missiles, Fire Blast, Frost Nova, Polymorph) |
| Warlock | — | — | [PARTIAL] | Dano baixo no teste (muito Life Tap); demônio por quest |
| Druid | [PARTIAL] | [PARTIAL] | [PARTIAL] | Formas; não testado |

## Dungeons

| Dungeon | Status |
|---|---|
| Ragefire Chasm | [PARTIAL] — 3 de 4 chefes mortos por bots sozinhos numa rodada; falta rota para não travar em mobs inalcançáveis |
| Outras | [BLOCKED] — sem mmaps (gerar por mapa) e sem rotas |

## Próximos passos (em ordem)

1. **Rotas da RFC:** pontos de rota até cada chefe, para o grupo só de bots não travar.
2. **Testar a cura de verdade:** grupo em nível igual ao dos mobs, sem vantagem de nível.
3. **AoE no chão e cast perigoso de chefe** (MechanicAI).
4. **Loot por roll** e wipe recovery completo.
5. Validar Paladin, Shaman, Druid e Hunter em grupos de teste.
6. Teste com o client: 1 jogador + 4 bots.
