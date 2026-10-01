/*
 * Forever bots: combat and movement AI, run from Player::Update (map thread) through the PlayerAI hook.
 * Spells are chosen by their English name and the best rank the bot knows, so the same lists work at every level.
 */

#include "ForeverBots.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ChaseMovementGenerator.h"
#include "Group.h"
#include "MotionMaster.h"
#include "Random.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    constexpr uint32 ThinkInterval = 400;
    constexpr uint32 OutOfCombatRegenDelay = 3000;
    constexpr float FollowDistance = 2.5f;
    constexpr float CasterRange = 22.0f;
    constexpr uint32 SpellWeakenedSoul = 6788;
    constexpr uint32 SpellAutoShot = 75;
}

ForeverBot* ForeverBotAI::GetBotData() const
{
    return sForeverBotMgr->GetBot(me->GetGUID());
}

Player* ForeverBotAI::GetOwner() const
{
    ForeverBot const* data = GetBotData();
    return data ? ObjectAccessor::GetPlayer(*me, data->Owner) : nullptr;   // same map only
}

std::vector<Player*> ForeverBotAI::GetGroupMembers(Player* owner) const
{
    std::vector<Player*> members;
    if (Group* group = me->GetGroup())
    {
        for (GroupReference const& itr : group->GetMembers())
            if (Player* member = itr.GetSource())
                if (member->IsInMap(me) && member->IsAlive())
                    members.push_back(member);
    }
    else
    {
        members.push_back(me);
        if (owner)
            members.push_back(owner);
    }
    return members;
}

bool ForeverBotAI::IsRanged() const
{
    return me->GetClass() == CLASS_HUNTER;
}

bool ForeverBotAI::IsCaster() const
{
    ForeverBot const* data = GetBotData();
    switch (me->GetClass())
    {
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            return true;
        case CLASS_DRUID:
        case CLASS_SHAMAN:
            return !data || data->Role != BotRole::Tank;
        case CLASS_PALADIN:
            return data && data->Role == BotRole::Heal;
        default:
            return false;
    }
}

SpellInfo const* ForeverBotAI::FindBestSpell(char const* spellName) const
{
    auto& cache = const_cast<ForeverBotAI*>(this)->_spellCache;
    if (_cachedLevel != me->GetLevel())
    {
        cache.clear();
        const_cast<ForeverBotAI*>(this)->_cachedLevel = me->GetLevel();
    }

    if (auto itr = cache.find(spellName); itr != cache.end())
        return itr->second ? sSpellMgr->GetSpellInfo(itr->second, DIFFICULTY_NONE) : nullptr;

    SpellInfo const* best = nullptr;
    for (auto const& [spellId, spell] : me->GetSpellMap())
    {
        if (spell.state == PLAYERSPELL_REMOVED || spell.disabled || !spell.active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info || !info->SpellName || info->IsPassive())
            continue;
        char const* name = (*info->SpellName)[LOCALE_enUS];
        if (!name || strcmp(name, spellName) != 0)
            continue;
        if (!best || info->SpellLevel > best->SpellLevel)
            best = info;
    }

    cache[spellName] = best ? best->Id : 0;
    return best;
}

bool ForeverBotAI::TryCast(Unit* target, char const* spellName, bool requireNoAura /*= false*/)
{
    if (!target || me->HasUnitState(UNIT_STATE_CASTING))
        return false;

    SpellInfo const* spellInfo = FindBestSpell(spellName);
    if (!spellInfo)
        return false;

    if (requireNoAura && target->HasAura(spellInfo->Id))
        return false;

    if (!me->GetSpellHistory()->IsReady(spellInfo) || me->GetSpellHistory()->HasGlobalCooldown(spellInfo))
        return false;

    for (SpellPowerCost const& cost : spellInfo->CalcPowerCost(me, spellInfo->GetSchoolMask()))
        if (me->GetPower(cost.Power) < cost.Amount)
            return false;

    if (target != me)
    {
        float range = spellInfo->GetMaxRange(spellInfo->IsPositive(), me);
        if (range < NOMINAL_MELEE_RANGE)
            range = NOMINAL_MELEE_RANGE;
        if (!me->IsWithinDistInMap(target, range) || !me->IsWithinLOSInMap(target))
            return false;
    }

    if (spellInfo->CalcCastTime() > 0 && me->isMoving())
        me->StopMoving();

    SpellCastResult result = me->CastSpell(target, spellInfo->Id, false);
    if (ForeverBot* data = GetBotData())
    {
        data->LastCastSpell = spellInfo->Id;
        data->LastCastResult = int32(result);
    }
    return result == SPELL_CAST_OK;
}

bool ForeverBotAI::DoHeal(std::vector<Player*> const& members)
{
    Player* lowest = nullptr;
    for (Player* member : members)
        if (member->GetHealthPct() < 95.0f && (!lowest || member->GetHealthPct() < lowest->GetHealthPct()))
            lowest = member;
    if (!lowest)
        return false;

    float const pct = lowest->GetHealthPct();
    switch (me->GetClass())
    {
        case CLASS_PRIEST:
            if (pct < 50.0f && lowest->IsInCombat() && !lowest->HasAura(SpellWeakenedSoul) && TryCast(lowest, "Power Word: Shield", true))
                return true;
            if (pct < 45.0f && (TryCast(lowest, "Flash Heal") || TryCast(lowest, "Heal") || TryCast(lowest, "Lesser Heal")))
                return true;
            if (pct < 80.0f && TryCast(lowest, "Renew", true))
                return true;
            return pct < 70.0f && (TryCast(lowest, "Heal") || TryCast(lowest, "Lesser Heal"));
        case CLASS_DRUID:
            if (pct < 85.0f && TryCast(lowest, "Rejuvenation", true))
                return true;
            if (pct < 45.0f && TryCast(lowest, "Regrowth", true))
                return true;
            return pct < 60.0f && TryCast(lowest, "Healing Touch");
        case CLASS_SHAMAN:
            if (pct < 45.0f && TryCast(lowest, "Lesser Healing Wave"))
                return true;
            return pct < 70.0f && TryCast(lowest, "Healing Wave");
        case CLASS_PALADIN:
            if (pct < 70.0f && TryCast(lowest, "Flash of Light"))
                return true;
            return pct < 60.0f && TryCast(lowest, "Holy Light");
        default:
            return false;
    }
}

bool ForeverBotAI::DoBuffs(std::vector<Player*> const& members)
{
    // self buffs
    switch (me->GetClass())
    {
        case CLASS_MAGE:
            if (TryCast(me, "Mage Armor", true) || (!FindBestSpell("Mage Armor") && (TryCast(me, "Ice Armor", true) || (!FindBestSpell("Ice Armor") && TryCast(me, "Frost Armor", true)))))
                return true;
            break;
        case CLASS_WARLOCK:
            if (TryCast(me, "Demon Armor", true) || (!FindBestSpell("Demon Armor") && TryCast(me, "Demon Skin", true)))
                return true;
            break;
        case CLASS_PRIEST:
            if (TryCast(me, "Inner Fire", true))
                return true;
            break;
        case CLASS_SHAMAN:
            if (TryCast(me, "Lightning Shield", true))
                return true;
            break;
        case CLASS_PALADIN:
            if (TryCast(me, "Devotion Aura", true))
                return true;
            break;
        case CLASS_HUNTER:
            if (TryCast(me, "Aspect of the Hawk", true) || (!FindBestSpell("Aspect of the Hawk") && TryCast(me, "Aspect of the Monkey", true)))
                return true;
            break;
        case CLASS_WARRIOR:
        {
            ForeverBot const* data = GetBotData();
            if (data && data->Role == BotRole::Tank && TryCast(me, "Defensive Stance", true))
                return true;
            break;
        }
        default:
            break;
    }

    // group buffs
    char const* groupBuff = nullptr;
    switch (me->GetClass())
    {
        case CLASS_PRIEST:  groupBuff = "Power Word: Fortitude"; break;
        case CLASS_MAGE:    groupBuff = "Arcane Intellect"; break;
        case CLASS_DRUID:   groupBuff = "Mark of the Wild"; break;
        case CLASS_PALADIN: groupBuff = "Blessing of Might"; break;
        default: break;
    }
    if (groupBuff)
        for (Player* member : members)
            if (TryCast(member, groupBuff, true))
                return true;
    return false;
}

bool ForeverBotAI::DoAttackSpells(Unit* target, ForeverBot const& data)
{
    bool const tank = data.Role == BotRole::Tank;
    bool const targetOnSomeoneElse = target->GetVictim() && target->GetVictim() != me;

    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            if (tank && targetOnSomeoneElse && TryCast(target, "Taunt"))
                return true;
            if (TryCast(me, "Battle Shout", true))
                return true;
            if (target->GetHealthPct() < 20.0f && TryCast(target, "Execute"))
                return true;
            if (tank && (TryCast(target, "Revenge") || TryCast(target, "Sunder Armor")))
                return true;
            if (!tank && (TryCast(target, "Overpower") || TryCast(target, "Rend", true)))
                return true;
            return me->GetPower(POWER_RAGE) >= 300 && TryCast(target, "Heroic Strike");
        case CLASS_PALADIN:
            if (TryCast(me, "Seal of Righteousness", true))
                return true;
            return TryCast(target, "Judgement");
        case CLASS_HUNTER:
            if (me->IsWithinMeleeRange(target))
                return TryCast(target, "Raptor Strike");
            if (!me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && me->HasSpell(SpellAutoShot))
            {
                SpellCastResult result = me->CastSpell(target, SpellAutoShot, false);
                const_cast<ForeverBot&>(data).LastCastSpell = SpellAutoShot;
                const_cast<ForeverBot&>(data).LastCastResult = int32(result);
            }
            if (TryCast(target, "Hunter's Mark", true) || TryCast(target, "Serpent Sting", true))
                return true;
            return TryCast(target, "Arcane Shot");
        case CLASS_ROGUE:
            if (me->GetPower(POWER_COMBO_POINTS) >= 3 && TryCast(target, "Eviscerate"))
                return true;
            return TryCast(target, "Sinister Strike");
        case CLASS_PRIEST:
            if (data.Role == BotRole::Heal && me->GetPowerPct(POWER_MANA) < 60.0f)
                return false;
            if (TryCast(target, "Shadow Word: Pain", true) || TryCast(target, "Mind Blast"))
                return true;
            return TryCast(target, "Smite");
        case CLASS_SHAMAN:
            if (data.Role == BotRole::Heal && me->GetPowerPct(POWER_MANA) < 60.0f)
                return false;
            if (TryCast(target, "Flame Shock", true) || TryCast(target, "Earth Shock"))
                return true;
            return TryCast(target, "Lightning Bolt");
        case CLASS_MAGE:
            if (TryCast(target, "Fire Blast"))
                return true;
            return TryCast(target, "Frostbolt") || TryCast(target, "Fireball");
        case CLASS_WARLOCK:
            if (me->GetPowerPct(POWER_MANA) < 25.0f && me->GetHealthPct() > 60.0f && TryCast(me, "Life Tap"))
                return true;
            if (TryCast(target, "Corruption", true) || TryCast(target, "Curse of Agony", true) || TryCast(target, "Immolate", true))
                return true;
            return TryCast(target, "Shadow Bolt");
        case CLASS_DRUID:
            if (data.Role == BotRole::Heal && me->GetPowerPct(POWER_MANA) < 60.0f)
                return false;
            if (TryCast(target, "Moonfire", true))
                return true;
            return TryCast(target, "Wrath");
        default:
            return false;
    }
}

Unit* ForeverBotAI::SelectTarget(ForeverBot const& data, Player* owner, std::vector<Player*> const& members) const
{
    if (data.Passive)
        return nullptr;

    auto valid = [this](Unit* unit) { return unit && unit->IsAlive() && unit->IsInMap(me) && me->IsValidAttackTarget(unit); };

    if (data.Order == BotOrder::Attack && owner)
        if (Unit* selected = owner->GetSelectedUnit(); valid(selected))
            return selected;

    // a tank picks up what hits the others first
    if (data.Role == BotRole::Tank)
        for (Player* member : members)
            if (member != me)
                for (Unit* attacker : member->getAttackers())
                    if (valid(attacker) && attacker->GetVictim() != me)
                        return attacker;

    // keep the current target (a world bot also before the fight starts: hopping to whatever is nearest
    // would leave its chase movement on the old target, standing still)
    if (Unit* victim = me->GetVictim(); valid(victim) && (victim->IsInCombat() || data.IsWorldBot()))
        return victim;

    if (owner)
        if (Unit* ownerVictim = owner->GetVictim(); valid(ownerVictim))
            return ownerVictim;

    for (Unit* attacker : me->getAttackers())
        if (valid(attacker))
            return attacker;

    for (Player* member : members)
        for (Unit* attacker : member->getAttackers())
            if (valid(attacker))
                return attacker;

    return nullptr;
}

void ForeverBotAI::MoveForCombat(Unit* target, ForeverBot const& data)
{
    if (data.Order == BotOrder::Stay)
    {
        if (me->isMoving())
            me->StopMoving();
        me->SetFacingToObject(target);
        return;
    }

    MotionMaster* motion = me->GetMotionMaster();
    if (data.Role == BotRole::Heal && !data.IsWorldBot())
        return;                             // the healer keeps following its owner during the fight

    if (IsCaster() || IsRanged())
    {
        if (me->IsWithinDistInMap(target, CasterRange) && me->IsWithinLOSInMap(target))
        {
            if (me->isMoving() && motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                me->StopMoving();
            if (!me->HasUnitState(UNIT_STATE_CASTING))
                me->SetFacingToObject(target);
            return;
        }
        if (!IsChasing(target))
            motion->MoveChase(target, CasterRange - 4.0f, 0.0f);
        return;
    }

    if (!IsChasing(target))
        motion->MoveChase(target);

    // melee swings need the target in front: a client turns its player, a bot has to be turned
    if (me->IsWithinMeleeRange(target) && !me->isMoving() && !me->HasInArc(float(M_PI) / 2, target))
        me->SetFacingToObject(target);
}

// a chase movement stops for good once its target is no longer our victim: it must follow the current target
bool ForeverBotAI::IsChasing(Unit* target) const
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
        return false;
    ChaseMovementGenerator const* chase = dynamic_cast<ChaseMovementGenerator const*>(me->GetMotionMaster()->GetCurrentMovementGenerator());
    return chase && chase->GetTarget() == target;
}

void ForeverBotAI::FollowOwner(Player* owner, ForeverBot const& data)
{
    if (data.Order == BotOrder::Stay || !owner)
        return;

    MotionMaster* motion = me->GetMotionMaster();
    if (motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        return;

    float const angle = float(M_PI) - 0.8f + data.Index * 0.55f;
    motion->Clear();
    motion->MoveFollow(owner, FollowDistance + data.Index * 0.8f, ChaseAngle(angle));
}

void ForeverBotAI::UpdateAI(uint32 diff)
{
    if (!me->IsAlive())
        return;

    if (_thinkTimer > diff)
    {
        _thinkTimer -= diff;
        return;
    }
    uint32 const elapsed = ThinkInterval - std::min(_thinkTimer, ThinkInterval) + diff;
    _thinkTimer = ThinkInterval;

    ForeverBot* data = GetBotData();
    if (!data || !data->Ready)
        return;

    Player* owner = data->IsWorldBot() ? nullptr : GetOwner();
    std::vector<Player*> members = data->IsWorldBot() ? std::vector<Player*>{ me } : GetGroupMembers(owner);

    // healers first look after the group (a world bot only after itself)
    if ((data->Role == BotRole::Heal || data->IsWorldBot()) && DoHeal(members))
        return;

    Unit* target = SelectTarget(*data, owner, members);

    // a world bot looks for something of its level to hunt once it is rested
    if (!target && data->IsWorldBot() && me->GetHealthPct() > 70.0f
        && (me->GetPowerType() != POWER_MANA || me->GetPowerPct(POWER_MANA) > 40.0f))
        target = FindHuntTarget();

    if (target)
    {
        _outOfCombatTime = 0;
        // melee swings for melee classes, and for a hunter whose target got into melee range (no damage otherwise)
        bool const melee = !IsCaster() && (!IsRanged() || me->IsWithinMeleeRange(target));
        if (me->GetVictim() != target || melee != me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            bool attacked = me->Attack(target, melee);
            data->LastTarget = target->GetName();
            data->LastAttackOk = attacked;
        }
        MoveForCombat(target, *data);
        if (!me->HasUnitState(UNIT_STATE_CASTING))
            DoAttackSpells(target, *data);
        return;
    }

    // no fight
    if (data->Order == BotOrder::Attack)
        data->Order = BotOrder::Follow;
    if (me->GetVictim())
        me->AttackStop();

    if (data->Role != BotRole::Heal && !data->IsWorldBot() && DoHeal(members))     // hybrid classes top the group off between fights
        return;

    if (data->IsWorldBot())
        Wander(*data);
    else
        FollowOwner(owner, *data);

    if (!me->IsInCombat())
    {
        RegenOutOfCombat(elapsed);
        if (!me->isMoving())
            DoBuffs(members);
    }
}

void ForeverBotAI::RegenOutOfCombat(uint32 elapsed)
{
    _outOfCombatTime += elapsed;
    if (_outOfCombatTime < OutOfCombatRegenDelay)
        return;

    // stands in for eating and drinking between pulls
    if (me->GetHealthPct() < 100.0f)
        me->ModifyHealth(me->CountPctFromMaxHealth(6));
    if (me->GetPowerType() == POWER_MANA && me->GetPowerPct(POWER_MANA) < 100.0f)
        me->ModifyPower(POWER_MANA, me->GetMaxPower(POWER_MANA) * 6 / 100);
}

namespace
{
    // creatures a world bot may hunt: alive, attackable, around its level, not elite, not someone else's fight
    struct HuntCheck
    {
        Player* Me;
        float Range;
        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || !Me->IsWithinDistInMap(creature, Range))
                return false;
            if (creature->IsCritter() || creature->IsElite() || creature->IsCivilian() || creature->IsTotem())
                return false;
            if (creature->GetCreatureTemplate()->npcflag)
                return false;
            if (creature->IsInCombat() && creature->GetVictim() != Me)
                return false;
            int32 const diff = int32(creature->GetLevel()) - int32(Me->GetLevel());
            if (diff < -5 || diff > 2)
                return false;
            return Me->IsValidAttackTarget(creature) && Me->IsWithinLOSInMap(creature);
        }
    };
}

Unit* ForeverBotAI::FindHuntTarget() const
{
    std::list<Creature*> found;
    HuntCheck check{ me, 50.0f };
    Trinity::CreatureListSearcher<HuntCheck> searcher(me, found, check);
    Cell::VisitGridObjects(me, searcher, 50.0f);

    Creature* nearest = nullptr;
    for (Creature* creature : found)
        if (!nearest || me->GetDistance(creature) < me->GetDistance(nearest))
            nearest = creature;
    return nearest;
}

void ForeverBotAI::Wander(ForeverBot const& data)
{
    if (me->isMoving() || me->HasUnitState(UNIT_STATE_CASTING))
        return;

    if (_wanderTimer > ThinkInterval)
    {
        _wanderTimer -= ThinkInterval;
        return;
    }
    _wanderTimer = urand(12000, 30000);

    // stroll around its hunting ground (a bit further out when it has been resting long)
    Position pos = me->GetRandomPoint(data.Home, 30.0f);
    me->GetMotionMaster()->MovePoint(1, pos);
}
