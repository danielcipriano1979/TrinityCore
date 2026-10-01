# ForeverBots — Papéis (Tank, Healer, DPS)

Código: `AI/BotCombat.cpp` (papéis e utilidade), `AI/BotMovement.cpp` (posição), `ForeverBotAI.cpp` (ordem das camadas).

## Origem do papel

- **Bot:** o papel escolhido no recrutamento (`.bot add <classe> [tank|heal|dps]`) fica em `ForeverBot::Role`. É a fonte da verdade, porque a spec dos dados Classic é sempre a de índice 0.
- **Jogador real:** é tratado como healer se for priest e como DPS nas outras classes. Se o tank do grupo for um jogador, ele aparece no blackboard (`Party.Tank`) quando um bot tank não existe.
- **Bot de mundo:** age como DPS que também se cura.

## Ordem das camadas num tick de combate

1. **Defensivos:** cooldown com vida abaixo de 35%; redução de ameaça se um mob está no bot e ele não é o tank.
2. **Interrupts:** cast inimigo interrompível, com reserva no blackboard.
3. **Dispels:** debuffs removíveis dos membros, com prioridade para os de controle (stun, fear, poly, silence…).
4. **Cura:** o healer sempre. Híbridos só curam em emergência e só quando o grupo não tem healer.
5. **Papel:** tank, DPS ou healer (o healer ajuda no dano com mana acima de 70%).
6. **Controle (CC):** só com 3 ou mais mobs, tank presente, e só bots DPS.
7. **Manutenção da classe:** posturas, formas, aspectos, selos, totens, pets.
8. **Movimento:** fora do cérebro; não interrompe casts.

## Tank

- **Alvo** (`SelectTankTarget`):
  1. mob batendo em outro membro, com o healer primeiro;
  2. o alvo atual;
  3. o mob onde a vantagem de ameaça do tank é menor;
  4. o alvo do pull.
- **Taunt:** qualquer magia `KIND_TAUNT` quando o alvo bate em outro jogador. A de área (Challenging Shout/Roar) entra com 3 ou mais mobs soltos.
- **Ameaça:** rotação de classe com prioridade de threat (Sunder/Revenge/Shield Slam; Maul/Swipe; Judgement/Consecration) e área com 2 ou mais mobs em volta. **Sem trapaça de ameaça.**
- **Posição:** corpo a corpo, de frente para o mob. Os outros melee vão para as costas.
- **Grupo só de bots:** o tank lidera. Puxa o mob mais próximo quando o grupo está pronto (vida ≥ 70%, mana do healer ≥ 60%, todos a até 35 jardas, ninguém lançando magia). Sem alvo perto, anda até o próximo pack e para a 25 jardas dele.

## Healer

- **Quem curar:** pontuação `vida% efetiva + distância/10`.
  - **Vida efetiva:** soma as curas em andamento no blackboard.
  - **Penalidades:** +30 se fora de alcance ou LOS; +10 se já há cura indo.
  - O tank ganha 3 pontos de vantagem.
- **Qual magia:**
  - **Crítico (< 35%):** a cura mais rápida, com relevância 70+.
  - **Normal:** a maior cura que não passa de 130% da vida que falta. Se nenhuma cabe, a menor (evita overheal).
  - **Escudo:** em combate, no tank ou em quem está abaixo de 70%, se não tem.
  - **HoT:** se falta mais de 10% e o alvo ainda não tem o HoT deste healer.
  - **Cura em área:** com 3 ou mais membros abaixo de 75% a até 30 jardas.
- **Mana:** abaixo de 20%, só cura quem está abaixo de 55%.
- **Posição:**
  - fica a até 28 jardas e com linha de visão do tank;
  - se um mob o ataca no corpo a corpo, vai até o tank para ele pegar o mob;
  - não persegue DPS fora da formação.

## DPS

- **Alvo** (`SelectTarget`), por pontuação: foco do grupo +100, alvo do tank +60, mob no healer +35, mob em mim +25, alvo atual +20, CC −500, vida baixa (até +15), distância −1 por jarda.
- **Abertura:** espera 1,5 s num mob recém-puxado que está no tank.
- **Ameaça:** com 90% ou mais da ameaça do tank no alvo, segura as habilidades (`HOLD_THREAT`). Se o mob vier para o bot, usa a magia de redução de ameaça (Fade, Feign Death).
- **Rotação:** perfil da classe e spec (`BOT_CLASS_SYSTEM.md`) + fallback genérico: o melhor dano por segundo de cast entre as magias conhecidas. Varinha para caster sem mana.
- **Posição:**
  - **Melee:** atrás do mob quando ele está em outro.
  - **Ranged/caster:** a 28–30 jardas e com linha de visão.
  - **Hunter:** sai da zona mínima de 8 jardas.

## Fora de combate

Ressurreição dos mortos (corpo, não fantasma), cura de quem está ferido, buffs que faltam, manutenção da classe e formação:
- o tank fica à frente do líder, o healer atrás, o DPS nos flancos;
- descanso sentado (comer/beber), recuperando ~4% por segundo até 95%.
