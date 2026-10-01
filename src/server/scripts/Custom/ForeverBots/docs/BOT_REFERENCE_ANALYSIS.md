# ForeverBots — Análise das referências

Projetos auditados em 2026-10-01, todos locais. Usamos **conceitos**. Nenhum arquivo foi copiado, por causa das licenças (abaixo).

## 1. Projetos

| Projeto | Onde | Linhagem / licença | Como o bot existe |
|---|---|---|---|
| **LegionMind-PlayerBots** | `D:\Jogos\Legion\LegionMind-PlayerBots` (extração de 17/09, a mais nova) | Próprio, sobre AshamaneCore 7.3.5 (TrinityCore), GPLv2 | Player real numa sessão sem socket; patch de core em 9 arquivos; tudo roda de `WorldScript::OnUpdate` → `PlayerBotMgr::UpdateBots` |
| AshamaneCore `PlayerBot` | `Legion\AshamaneCore(-s2)\src\server\scripts\Custom\PlayerBot` | Versões anteriores do LegionMind | Igual |
| **mod-playerbots** | `D:\Jogos\Legion\backup\mod-playerbots-master` | AzerothCore (liyunfan ← ike3/cmangos). GPLv2 na maioria dos arquivos, **mas 11 arquivos são AGPLv3**, entre eles `PlayerbotAI.cpp` | Player real em sessão sem socket; hooks próprios no core AzerothCore |
| **CoA bots** | `D:\Jogos\CoA-Repack-0925\CoA-Bots` | mod-playerbots + camada "Coa" (~70 classes `Coa*`) só em binário; config, SQL e addon presentes | Igual ao mod-playerbots |
| SquidBotsLite (addon CoA) | `CoA-Bots\Client\Interface\AddOns\SquidBotsLite` | GPL-2.0 | Comandos de chat (`co +passive`, `@tank nc -coa auto pull`) |
| SPP-LegionV2 / sppv2 | `D:\Jogos\Legion\SPP-LegionV2`, `sppv2` | Binário | **Sem módulo de bots** (só AHBot) |
| NPCBots | — | — | **Não usado** em nenhum dos repacks (nenhum `bot_ai`/`botmgr`) |

## 2. Sistemas, comparados

| Sistema | mod-playerbots / CoA | LegionMind | O que adotamos |
|---|---|---|---|
| Motor de decisão | Triggers → ações com relevância numa fila de prioridade; Alternatives/Prerequisites/Multipliers; 3 motores (combate, fora de combate, morto) | Fila de relevância por tick (tank/healer) + `if/return` no DPS | **Fila de candidatos com relevância** + estados (DEAD/RECOVER/COMBAT/IDLE). Os multiplicadores viram "vetos" do DungeonAI/MechanicAI na fase 6/7 |
| Magias | Por **nome em inglês** (rank mais alto, cache de 20 s). O CoA soma um **classificador de SpellInfo em runtime** + rotações autorais por cima | **IDs** numa tabela SQL `playerbot_spells` com colunas de condição | **Classificador de SpellInfo** (CoA) + perfis por nome (já usado aqui, robusto com os ranks do Classic). Sem tabela de IDs (proibido inventar IDs, e o Classic tem vários ranks) |
| Papel / spec | Árvore com mais pontos (`GetPlayerSpecTab`) | Spec fixa por classe/papel (Legion) | Papel escolhido no recrutamento; spec inferida da árvore do trait config |
| Cura | `vida% + distância/10`, +30 se já há cura indo; cancela cura se o alvo encheu | Mesma fórmula, +30 fora de alcance/LOS; ignora ≥99%; sem previsão de cura | Mesma fórmula + **curas em andamento no blackboard** + HoT ativo + eficiência pela cura estimada |
| Ameaça | `ThreatValue` contra o topo; multiplicador "threat" reduz DPS; tank pega o mob com menos ameaça nele | **Trapaça**: `AddThreat` a cada 500 ms; DPS nunca checa ameaça | Sem trapaça. DPS pausa com ≥90% da ameaça do tank; tank prioriza mob solto e o de menor ameaça nele |
| Interrupt | Trigger por bot, **sem arbitragem** | `_kickClaims`: um bot reserva o cast por 1,2 s | **Reserva no blackboard** (reservado / backup) |
| Dispel | Triggers de cura por tipo | Só priest (527) fixo | Classificação por `DispelType` do SpellInfo, prioridade por perigo do debuff |
| CC | `CcTargetValue` | Só Polymorph em mob que bate no healer | Por aura (stun/fear/poly/root/banish), em alvo marcado ou add perigoso, nunca no alvo do grupo |
| Movimento | `MoveTo/ReachCombatTo/MoveToLOS` com `PathGenerator`; formações; triggers de stuck | Melee atrás (`MoveChase` com offset de reach); caster com zona morta 8–14 jardas; stuck detector 2 s/5 s; teleporte de resgate 40/80 jardas | Posição por papel (tank de frente, melee atrás, ranged a 20–25 jardas com LOS, healer perto do tank), stuck detector, regroup sem teleporte em luta |
| Pull / dungeon | `PullStrategy`, estratégias por raid (MC, …) | Prontidão pelo tank (≥75% vida, ≥65% mana, ≤35 jardas); rotas gravadas; auto-marcação | Prontidão pelo tank (fase 2) + DungeonAI com InstanceScript (fase 6) |
| Morte / wipe | Release, revive, aceitar ressurreição | Espera o healer; corre até o corpo quando o dono solta | Aceitar ressurreição, ressurreição por healer fora de luta, wipe → regroup (fase 8) |
| Loot | Roll por `ItemUsage` | Roll Need/Greed por upgrade | Roll por upgrade/uso (fase 8, precisa de acessor no core) |
| Comandos | ~136 comandos de chat | Comandos GM `.bot …` + addon por whisper | `.bot …` + addon ForeverBots (já existe), com comandos novos (debug, test, foco, pull) |
| Performance | Intervalos por trigger/value; bots longe de jogadores ficam mais lentos | Tick 500 ms, caches de alcance | Tick rápido/lento, contexto por tick, blackboard compartilhado |
| Telemetria | `CoaGroupTelemetry` (tempo <50%/25%, overheal, mana) | JSON de status a cada 3 s | Contadores por bot (dano, cura, overheal, mortes, interrupts, dispels) via hooks `DamageDealt`/`HealDone` |

## 3. Licenças

- **mod-playerbots:** GPLv2 na maioria dos arquivos, mas `PlayerbotAI.cpp` e outros 10 são AGPLv3. Copiar esses arquivos para o nosso core (GPLv2) obrigaria o servidor inteiro a seguir a AGPLv3, inclusive oferecer o código-fonte a quem joga. **Não copiamos código, só ideias.**
- **LegionMind:** é nosso (GPLv2, derivado de TrinityCore), mas tem APIs de Legion (specs, artefatos, Mythic+), `AddThreat` artificial e hacks por dungeon. **Ideias e fórmulas, não código.**
- **CoA:** só binário; usamos o que a config documenta.

## 4. APIs que não batem com o nosso core

| Referência | Nosso core (TrinityCore master) |
|---|---|
| `getThreatManager().getThreat()` / `GetThreatMgr()` | `GetThreatManager().GetThreat(Unit*)`, `GetCurrentVictim()`, `GetSortedThreatList()` |
| `CastSpell(target, id, false)` | `CastSpell(target, id, CastSpellExtraArgs)` (o `false` ainda converte) |
| `spellInfo->Effects[i]` | `GetEffects()` / `GetEffect(EFFECT_n)` |
| `sSpellMgr->GetSpellInfo(id)` | precisa de `Difficulty` |
| `GetPlayerSpecTab` (talent tab) | trait config (`ActiveCombatTraitConfigID`) → árvore com mais pontos |
| `HasFlag(UNIT_FIELD_FLAGS…)` | `HasUnitFlag`, `HasPlayerFlag` |
| `GetFirstMember()` | range-for em `GetMembers()` / `GetMemberSlots()` |
| Phase masks | `PhasingHandler` |
| Hooks `PlayerbotScript` do AzerothCore | Já temos os equivalentes (`SetBotSession`, `LoginBotCharacter`, `FinishBotTeleport`) |

## 5. Melhor abordagem para o Forever

1. Manter a **infra de sessão e login** que já funciona aqui. Ela resolve o que o patch 0001 do LegionMind resolve e respeita as limitações do client 1.60.
2. Trocar a AI monolítica por **camadas que propõem candidatos** num cérebro de relevância, com a classificação de magias do SpellInfo e perfis de classe por nome.
3. **Coordenação pelo blackboard do grupo** (interrupt, CC, curas em andamento, foco), o que nem o mod-playerbots tem de forma nativa.
4. Dungeon e mecânicas entram como **vetos/boosts** de relevância (o conceito de multiplicadores), lendo o InstanceScript em vez de duplicar a lógica dos chefes.
5. Um **modo de teste sem client** (grupo só de bots com líder bot) para validar comportamento no servidor de teste antes de pedir teste no jogo.
