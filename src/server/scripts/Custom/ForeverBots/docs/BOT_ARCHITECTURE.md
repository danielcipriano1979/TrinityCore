# ForeverBots — Arquitetura

Documento técnico do sistema de bots do servidor WoW Forever local. Explica o estado atual (auditoria de 2026-10-01), as restrições do core e a arquitetura nova (Party AI). O estado de cada parte está em `BOT_IMPLEMENTATION_STATUS.md`.

## 1. Base

| Item | Valor |
|---|---|
| Core | TrinityCore **master**, fork `advocaite/TrinityCore` branch `forever`, mais o nosso branch `local-fixes` |
| Client | WoW Classic 1.60.1.70124 (WoW Forever beta), protocolo moderno (UpdateFields, Traits) com conteúdo vanilla |
| Classes | As 9 do Classic: Warrior, Paladin, Hunter, Rogue, Priest, Shaman, Mage, Warlock, Druid (`sObjectMgr->GetPlayerInfo(race, class)`) |
| Specs | `ChrSpecialization` existe, mas o Classic não tem specs "iniciais". `GetDefaultChrSpecializationForClass` cai no índice 0, então **todo** priest é Discipline e todo warrior é Arms. A spec real vem da árvore de talentos onde o bot gastou pontos |
| Talentos | Trait config (`C_ClassTalents`), validada por `TraitMgr::ValidateConfig` |
| Pathfinding | `mmap.enablePathFinding = 1`, mas só existe mmap da Ragefire Chasm (389). No resto o `PathGenerator` devolve linha reta (`NOT_USING_PATH`) e não detecta alvo inalcançável |

## 2. Como um bot existe (já implementado, reaproveitado)

Um bot é um **`Player` real** num `WorldSession` sem socket (`WorldSession::SetBotSession`). O manager é dono dessas sessões, que não ficam no mapa de sessões do `World`.

| Peça | Onde | Observação |
|---|---|---|
| Sessão sem socket | `WorldSession.h:991-996`, `.cpp:225/360/552/565/621` | `SendPacket` é no-op; sem idle kick; tokens de teleporte finalizados no servidor |
| Login | `CharacterHandler.cpp:1181` (`LoginBotCharacter`) e hook antes de `AddPlayerToMap` (`:1343`) | `PrepareBeforeWorld`: nível, spells, talentos, gear e grupo **antes** de ficar visível |
| Teleporte | `MovementHandler.cpp:471-500` (`FinishBotTeleport`) | Emula os acks do client |
| AI | `Player::SetBotAI` → `PlayerAI`, chamado por `Unit::AIUpdateTick` na **thread do mapa** | Trocar de AI dentro de `UpdateAI` dispara assert |
| Manager | `ForeverBotMgr::Update` em `WorldScript::OnUpdate` (thread do world, depois dos mapas) | Login, relog, remoção, bots de mundo |

Restrições do client 1.60 que a arquitetura precisa respeitar:
1. Um update completo de valores de membro de grupo sobre um jogador **visível** derruba o client. Mudanças pesadas (grupo, nível, gear) só acontecem no login ou por relog.
2. `SendPacket` não faz nada para bots, então tudo que o client faria tem de ser feito no servidor: aceitar ressurreição, rolar loot, ack de teleporte.
3. O bot é cego até `PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME` (`Player::CanNeverSee`).
4. Personagens só podem ser gravados de forma assíncrona: o banco de characters não tem statements síncronos.

## 3. Auditoria do código atual (antes da reescrita)

`ForeverBotAI` (561 linhas) é uma AI monolítica:
- **Tick:** a cada 400 ms. Primeiro cura, depois escolhe alvo, ataca, se move e lança as magias de dano. Fora de combate segue o dono, regenera e passa buffs.
- **Magias:** escolhidas pelo nome em inglês, com o melhor rank conhecido. As listas são fixas por classe (`DoAttackSpells`, `DoHeal`, `DoBuffs`).
- **Cura:** o membro com menos vida abaixo de 95%, com limiares fixos.
- **Alvo:** ordem fixa: ordem de ataque → atacantes dos outros membros (só o tank) → vítima atual → vítima do dono → atacantes.
- **Movimento:** casters param a 22 jardas, melee usa `MoveChase`. O healer segue o dono durante a luta.
- **Não existe:** threat (além do Taunt), interrupt, dispel, CC, ressurreição, pets, totens, formas, desvio de AoE, raid markers, loot, pulls, conhecimento de dungeon.

## 4. Arquitetura nova

### 4.1 Princípios

- **Prioridade dinâmica, não rotação fixa.** A cada tick cada camada **propõe** ações com relevância. O cérebro executa a mais relevante que for possível agora. Se uma ação falha, a próxima é tentada no mesmo tick, sem desperdiçar o tick. Isso vem do *relevance queue* do mod-playerbots e do LegionMind.
- **Magias vêm do SpellInfo.** O `BotSpellBook` classifica as magias que o bot conhece pelos efeitos e auras: cura, HoT, escudo, dano, DoT, AoE, interrupt, dispel (por tipo), taunt, CC, ressurreição, buff e defensivo. Os perfis de classe só **ordenam e ajustam** (conhecimento de rotação, por nome). Interrupt, dispel, taunt e ressurreição nunca dependem de lista fixa.
- **O grupo é uma entidade.** Um `PartyBlackboard` por grupo guarda os papéis, o alvo do pull, o alvo foco, as reservas de interrupt, os alvos de CC e as curas em andamento. Isso evita 5 bots interrompendo o mesmo cast ou dois healers curando a mesma pessoa.
- **Leve.** Duas cadências por bot. O tick rápido (combate, padrão 250–400 ms com variação por personalidade) lê o contexto do tick. O tick lento (2 s) reclassifica o spellbook, reavalia papéis e gear e limpa o quadro do grupo. Buscas na grade só no tick lento ou quando não há luta.

### 4.2 Camadas

```
ForeverBotAI (PlayerAI, thread do mapa)       -- controller fino: monta o contexto e roda o cérebro
 ├─ BotContext        snapshot do tick: eu, dono/líder, membros (papel, vida, mana, distância),
 │                     inimigos em luta com o grupo, atacantes, alvo atual, estado de instância/chefe
 ├─ BotBrain          coleta Candidate{relevância, tipo, alvo, executor, motivo}, ordena, executa;
 │                     guarda a decisão atual e o motivo (debug/telemetria)
 ├─ BotSpellBook      classificação das magias conhecidas pelo SpellInfo (rank mais alto), cache por nível
 ├─ ClassProfile      conhecimento por classe/papel (prioridades de rotação, buffs, postura/forma)
 ├─ RoleAI            TankRole / HealerRole / DpsRole: propõem as ações do papel
 ├─ UtilityAI         interrupt, dispel, CC, buffs, ressurreição, defensivos, consumíveis
 ├─ TargetAI          seleção de alvo com pontuação (marcação, foco, perigo, ameaça, vida, distância)
 ├─ ThreatAI          % de ameaça do bot contra o tank no alvo; segura DPS; detecta perda de aggro
 ├─ MovementAI        follow em formação, posição de combate por papel, LOS, distância, stuck, regroup
 ├─ PartyAI           PartyBlackboard por grupo: papéis, líder, foco, pull, reservas, estado da luta
 ├─ DungeonAI         (fase 6) contexto da instância, chefes via InstanceScript, pulls, progressão
 ├─ MechanicAI        (fase 7) AoE no chão (DynamicObject/AreaTrigger), casts perigosos de chefe
 └─ RecoveryAI        morte, aceitar ressurreição, wipe, regroup, comer/beber
```

### 4.3 Ciclo de decisão (tick rápido)

1. **Contexto:** monta o `BotContext`, com dados de grupo compartilhados no blackboard por ~250 ms.
2. **Estado:** `DEAD`, `RECOVER` (sem luta, recursos baixos), `COMBAT` ou `IDLE` (seguir/esperar).
3. **Propostas:** `RecoveryAI` → `MechanicAI` → `UtilityAI` → `RoleAI` → `MovementAI` propõem `Candidate`s.
4. **Execução:** o `BotBrain` ordena por relevância e executa a primeira proposta que der certo. Uma falha por GCD, alcance ou mana não gasta o tick.
5. **Movimento:** roda separado das magias, para não brigar com casts. Movimento que interrompe cast só acontece com relevância de mecânica (sair de AoE).

Escala de relevância, compatível com o mod-playerbots:

| Banda | Valor | Exemplos |
|---|---|---|
| EMERGENCY | 90 | sair de AoE, cura de emergência no tank, defensivo com vida crítica |
| CRITICAL_HEAL | 70 | cura de quem está abaixo de 35% |
| TAUNT | 60 | tank perdeu um mob para um não-tank |
| DISPEL | 50 | debuff perigoso (magic/curse/poison/disease) |
| INTERRUPT | 45 | cast inimigo interrompível com reserva do grupo |
| HEAL | 40 | cura normal, HoT, escudo |
| CC | 35 | CC em alvo marcado (moon/square) ou add perigoso |
| ROTATION | 10–30 | rotação de dano e threat, por prioridade do perfil |
| BUFF | 8 | buff faltando fora de luta |
| MOVE | 5 | posicionar, seguir, reagrupar |
| IDLE | 0 | nada a fazer |

### 4.4 Papéis

- **Tank.** Prioridades: sobreviver (defensivo) → mob solto (atacando healer, depois DPS) = taunt/ataque → manter o alvo principal → threat em AoE com ≥3 inimigos → rotação. Posição: de frente para o mob, o grupo atrás dele. Puxa só quando o grupo está pronto: vida ≥ 75%, mana do healer ≥ 65%, todos a menos de 35 jardas.
- **Healer.** O alvo é escolhido por pontuação: `vida% + distância/10 + 30 se fora de alcance/LOS + 25 se outra cura já está indo nele + 10 se tem HoT`. O tank ganha prioridade no empate. A magia é escolhida pela urgência (emergência → a mais rápida) e pela eficiência (cura estimada ≤ vida que falta × 1,3, para não desperdiçar). Guarda mana com mana baixa. Mantém distância do inimigo e LOS com o tank, e não persegue DPS fora da formação.
- **DPS.** Alvo: foco do grupo / marca skull → alvo do tank → mob em quem o tank já gerou ameaça (o DPS espera ~1,5 s de abertura) → mais próximo em luta com o grupo. Com ameaça ≥ 90% da do tank no alvo, pausa as ações de dano alto e usa redução de ameaça se a classe tiver. AoE com ≥3 inimigos perto do alvo e nenhum CC por perto.

### 4.5 Comunicação (PartyBlackboard)

Um por grupo, chaveado pelo GUID do grupo (ou do dono, sem grupo).

| Chave | Conteúdo |
|---|---|
| `Tank`, `Healers` | GUIDs por papel (dos bots e dos jogadores reais, estes inferidos pela classe/árvore) |
| `Leader` | o jogador líder (ou o tank bot, no modo autônomo) |
| `FocusTarget` | definido pelo jogador (`.bot attack`, marca skull) ou pelo tank |
| `PullTarget` | alvo que o tank está puxando |
| `InterruptClaims` | alvo → (bot, spell, expira) |
| `CcTargets` | alvo → (bot, spell) |
| `IncomingHeals` | alvo → (cura estimada, expira) |
| `CombatState` | IDLE / PULLING / COMBAT / RECOVERY / WIPE |

### 4.6 Integração com o core

- **Leitura direta:** `ThreatManager` do inimigo (`GetThreat`, `GetCurrentVictim`, `IsThreatenedBy`), `GetThreatenedByMeList` do bot, `Unit::GetCurrentSpell`, `SpellInfo::CanBeInterrupted`, `GetDispellableAuraList`, `InstanceScript::GetBossState`.
- **Ações:** `CastSpell` + `CastSpellExtraArgs`, `MotionMaster` (`MoveChase` com `ChaseRange`/`ChaseAngle`, `MovePoint`, `MoveFollow`), `PathGenerator` (só onde há mmap).
- **Pequenos acessores no core (fase 5/8):** getter dos ícones de alvo do grupo, que hoje são privados, e getter dos rolls de loot pendentes.
- **Não fazemos:** trapaça de ameaça (`AddThreat` artificial, como o LegionMind faz) e teleporte em luta.

## 5. Limitações e riscos

| Risco | Mitigação |
|---|---|
| Sem mmaps fora da RFC: bots e mobs andam em linha reta | Gerar mmaps de Kalimdor/Eastern Kingdoms (horas de CPU, sem mudança de código); detecção de stuck + LOS enquanto isso |
| Spec "real" não existe nos dados | Spec inferida da árvore com mais pontos; papel do bot (`ForeverBot::Role`) é a fonte da verdade |
| Client cai com update de membro visível | Mantido o modelo de relog para mudanças pesadas |
| Pets (hunter/warlock) | Fase 3: invocar pet por magia (hunter já tem `Call Pet` se tiver pet; warlock `Summon Imp`); validar com socket-less |
| Thread do mapa | AI só lê `_bots`; mutações de manager continuam na thread do world |
| Testes sem client | Comando `.bot test` (fase 1): grupo só de bots com líder bot, para rodar cenários no servidor de teste |

## 6. O que é reaproveitado, adaptado e reescrito

| Sistema | Decisão |
|---|---|
| Sessão, login, relog, teleporte, criação de personagem, gear, talentos | **Reaproveitado** (`ForeverBotMgr`) |
| Busca de magia por nome (melhor rank) | **Adaptado** para o `BotSpellBook` (classificação por efeito + nome) |
| `DoHeal` / `DoAttackSpells` / `DoBuffs` | **Reescrito** em HealerRole / ClassProfile / UtilityAI |
| `SelectTarget` | **Reescrito** em TargetAI com pontuação |
| `MoveForCombat` / `FollowOwner` | **Reescrito** em MovementAI (posição por papel, stuck) |
| Regen artificial de 6%/tick | **Removido** na fase 8 (comer/beber de verdade); fica reduzido até lá |
