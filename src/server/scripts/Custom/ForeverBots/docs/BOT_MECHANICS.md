# ForeverBots — Mecânicas

## O que já reage a mecânicas

| Mecânica | Resposta | Onde |
|---|---|---|
| Inimigo lançando magia interrompível | Interrupt, com reserva de 1,5 s no blackboard: um bot interrompe e os outros ficam de reserva | `ProposeInterrupts` |
| Debuff removível em aliado | Dispel; prioridade maior para controle (stun, fear, poly, charm, sleep, silence, horror, root) e no tank | `ProposeDispels` |
| Mob solto batendo em healer ou DPS | Taunt do tank (de área com 3 ou mais mobs) | `ProposeTank` |
| DPS perto da ameaça do tank | Segura as habilidades (≥ 90%); se tomar o aggro, usa redução de ameaça | `ProposeDps`, `ProposeDefensives` |
| Vida crítica | Defensivo do spellbook (escudo, imunidade, redução de dano) | `ProposeDefensives` |
| Mob no corpo a corpo do mage | Frost Nova | perfil Mage |
| Mob no corpo a corpo do healer | O healer vai até o tank, para ele pegar o mob | `MoveInCombat` |
| Mob sob controle | Ninguém bate (pontuação −500) e o CC fica reservado no blackboard | `SelectTarget`, `ProposeCrowdControl` |
| Pack com 3 ou mais mobs | Área: Thunder Clap, Swipe, Consecration, Arcane Explosion, Blizzard, Rain of Fire, Multi-Shot… | perfis de classe |

## O que falta (fase 7)

| Mecânica | Como detectar | Resposta prevista |
|---|---|---|
| AoE no chão | `DynamicObject` / `AreaTrigger` hostil num raio de ~15 jardas (grid searcher, só no tick lento ou quando há luta) | Sair para o raio + 3 jardas, num ponto alcançável |
| Cone frontal / cleave | Chefe com cast de `SpellInfo` de alvo em cone | Melee fica atrás; o tank vira o chefe para longe do grupo |
| Knockback | Aura/efeito de knockback do chefe | Tank encostado numa parede quando possível; voltar à posição |
| Fear | Debuff de fear em aliado | Dispel (já existe) / Tremor Totem / Fear Ward |
| Fases e enrage | `InstanceScript`, auras de enrage (`DISPEL_ENRAGE`) | Cooldowns defensivos, Tranquilizing Shot |
| Adds | Novo inimigo em combate durante o chefe | O DPS troca para o add se a pontuação do add for maior (marca, "no healer") |

Por regra (`docs/BOT_ARCHITECTURE.md`), a lógica do chefe não é duplicada: o bot lê o `InstanceScript`, o cast atual do chefe e as auras.
