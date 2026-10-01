# ForeverBots — Classes e specs

Código: `AI/BotCore.cpp` (classificação de magias) e `AI/BotClassAI.cpp` (conhecimento por classe).

## Classes disponíveis

As 9 do Classic 1.60, confirmadas pelos dados (`sObjectMgr->GetPlayerInfo(race, class)`): Warrior, Paladin, Hunter, Rogue, Priest, Shaman, Mage, Warlock, Druid. **Não existem:** Death Knight, Monk, Demon Hunter, Evoker.

## Duas camadas

1. **Spell book classificado (genérico, todas as classes).** Para cada magia conhecida (o rank mais alto de cada nome), os efeitos e as auras do `SpellInfo` definem os tipos:

| Tipo | Como é detectado |
|---|---|
| Cura direta / em área | `SPELL_EFFECT_HEAL`, `HEAL_PCT` (área se `IsAffectingArea`) |
| HoT | aura `PERIODIC_HEAL` |
| Escudo | aura `SCHOOL_ABSORB` positiva |
| Dano / DoT / área | `SCHOOL_DAMAGE`, dano de arma, auras `PERIODIC_DAMAGE/LEECH` |
| Interrupt | `SPELL_EFFECT_INTERRUPT_CAST` |
| Dispel / Purge | `SPELL_EFFECT_DISPEL` (máscara pelo `DispelType`), aliado ou inimigo pelo alvo |
| Taunt | `SPELL_EFFECT_ATTACK_ME`, aura `MOD_TAUNT` |
| Controle | auras `MOD_STUN/FEAR/CONFUSE/CHARM/TRANSFORM`, imunidade em inimigo (Banish) |
| Ressurreição | `SPELL_EFFECT_RESURRECT` |
| Buff / buff próprio | aura positiva longa (≥ 2 min ou infinita) sem cooldown, exceto as exclusivas (`GetSpellSpecific` ≠ normal) |
| Defensivo | absorção/imunidade só no caster com cooldown ≥ 20 s, `MOD_DAMAGE_PERCENT_TAKEN` negativo |
| Redução de ameaça | `MODIFY_THREAT_PERCENT` negativo, `MOD_TOTAL_THREAT` negativo, Feign Death |

Isso cobre interrupt, dispel, taunt, CC, ressurreição, buffs e defensivos **sem nenhuma lista fixa de IDs**.

2. **Perfil de classe (por nome, rank mais alto).** O que precisa de conhecimento: posturas, formas, aspectos, selos, auras, armaduras, imbuições, totens, pets e a prioridade de dano/ameaça. A spec é lida pelas magias de talento conhecidas.

| Classe | Manutenção | Tank | Dano | Spec detectada por |
|---|---|---|---|---|
| Warrior | Defensive/Battle/Berserker Stance, Battle Shout | Shield Slam, Revenge, Thunder Clap, Demoralizing Shout, Sunder, Cleave/Heroic Strike | Charge, Execute, Overpower, Mortal Strike/Bloodthirst, Whirlwind, Rend, Heroic Strike/Cleave | Mortal Strike / Bloodthirst |
| Paladin | Righteous Fury (tank), Devotion/Retribution Aura, Seal of Command/Righteousness, Blessing of Might/Wisdom | Judgement, Consecration | Judgement, Exorcism, Hammer of Wrath | Seal of Command |
| Hunter | Aspect of the Hawk/Monkey, Auto Shot | — | Hunter's Mark, Aimed Shot, Multi-Shot, Serpent Sting, Arcane Shot; Raptor Strike/Mongoose Bite no melee | Aimed Shot |
| Rogue | — | — | Slice and Dice, Eviscerate, Backstab (pelas costas), Sinister Strike/Hemorrhage | Hemorrhage |
| Priest | Shadowform (DPS shadow) | — | Shadow Word: Pain, Vampiric Embrace, Mind Blast, Mind Flay, Holy Fire, Smite | Shadowform |
| Shaman | Lightning Shield, imbuição de arma, totens de terra/fogo/água | — | Stormstrike, Flame Shock, Earth Shock, Chain Lightning, Lightning Bolt | Stormstrike |
| Mage | Mage/Ice/Frost Armor | — | Frost Nova (melee em mim), Arcane Explosion, Blizzard (em área), Evocation, Fire Blast, Pyroblast, Arcane Missiles, Frostbolt/Fireball | Pyroblast |
| Warlock | Demon Armor/Skin, demônio (Voidwalker sem tank, senão Imp) | — | Life Tap, Drain Life, Corruption, Curse of Agony, Immolate, Rain of Fire, Shadow Bolt | — |
| Druid | Bear/Dire Bear Form (tank), Cat Form (feral) | Swipe, Demoralizing Roar, Maul, Enrage | Cat: Rip/Ferocious Bite, Rake, Shred, Claw. Caster: Moonfire, Insect Swarm, Starfire, Wrath | Cat Form / Insect Swarm |

A cura de Priest, Paladin, Shaman e Druid vem toda da camada genérica (classificação + escolha por eficiência).

## Regras de cast

- Checagens na ordem: magia conhecida, nada sendo lançado, magia não bloqueada, cooldown, GCD, poder, alcance (melee: `IsWithinMeleeRange`), linha de visão.
- O bot vira para o alvo (o servidor recusa magias e golpes de costas) e para de andar antes de magias com tempo de cast.
- Um cast recusado pelo servidor bloqueia aquela magia por 2,5 s. Alcance, LOS, "not ready" e "in front" não bloqueiam, porque se resolvem andando. É a regra anti-loop de cast do LegionMind.
- Auto Shot e varinha são autorepeat: iniciados uma vez, não são enfileirados.
