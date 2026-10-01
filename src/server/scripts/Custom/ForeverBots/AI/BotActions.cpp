/*
 * Forever bots: casting. Every cast is checked the way the client would (known, cooldown, global cooldown, power,
 * range, line of sight, facing). A cast the server refuses blocks that spell for a short while, so a bot never
 * loops on a spell it can't cast (the LegionMind "cast loop" rule); cooldowns and range don't count as refusals.
 */

#include "BotAI.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

using namespace ForeverBots;

namespace
{
    constexpr uint32 RefusedCastGrace = 2500;
}

bool ForeverBotAI::IsSpellBlocked(uint32 spellId, uint32 now) const
{
    auto itr = _blockedSpells.find(spellId);
    return itr != _blockedSpells.end() && itr->second > now;
}

bool ForeverBotAI::CanCast(BotSpell const* spell, Unit* target, bool checkRange /*= true*/) const
{
    if (!spell || !spell->Info || !target)
        return false;
    if (me->HasUnitState(UNIT_STATE_CASTING) || me->IsNonMeleeSpellCast(false))
        return false;
    if (IsSpellBlocked(spell->Info->Id, GameTime::GetGameTimeMS()))
        return false;
    // the stance / form / stealth and the weapon the spell needs (Revenge, Garrote, Backstab, shots...)
    if (spell->Info->CheckShapeshift(me->GetShapeshiftForm()) != SPELL_CAST_OK || !me->HasItemFitToSpellRequirements(spell->Info))
        return false;

    SpellHistory* history = me->GetSpellHistory();
    if (!history->IsReady(spell->Info) || history->HasGlobalCooldown(spell->Info))
        return false;

    for (SpellPowerCost const& cost : spell->Info->CalcPowerCost(me, spell->Info->GetSchoolMask()))
        if (me->GetPower(cost.Power) < cost.Amount)
            return false;

    // procs and states: Revenge after a dodge, Execute under 20%, Overpower...
    if (spell->Info->CasterAuraState && !me->HasAuraState(AuraStateType(spell->Info->CasterAuraState), spell->Info, me))
        return false;
    if (spell->Info->TargetAuraState && target != me && !target->HasAuraState(AuraStateType(spell->Info->TargetAuraState), spell->Info, me))
        return false;

    if (checkRange && target != me)
    {
        if (float minRange = spell->Info->GetMinRange(spell->Info->IsPositive()); minRange > 0.0f && me->IsWithinDistInMap(target, minRange))
            return false;   // shots have a minimum range
        float range = spell->Info->GetMaxRange(spell->Info->IsPositive(), me);
        if (spell->Has(KIND_MELEE) || range < NOMINAL_MELEE_RANGE)
        {
            if (!me->IsWithinMeleeRange(target))
                return false;
        }
        else if (!me->IsWithinDistInMap(target, range))
            return false;
        if (!me->IsWithinLOSInMap(target))
            return false;
    }
    return true;
}

bool ForeverBotAI::Cast(BotSpell const* spell, Unit* target)
{
    if (!CanCast(spell, target))
        return false;

    // the server only lands spells and swings on a target in front: a client turns its player, a bot is turned here
    if (target != me && !me->HasInArc(float(M_PI) / 2, target))
        me->SetFacingToObject(target);
    if (spell->CastTime > 0 && me->isMoving())
    {
        me->GetMotionMaster()->Clear();
        me->StopMoving();
    }

    bool const targetWasCasting = target != me && target->IsNonMeleeSpellCast(false, false, true);
    SpellCastResult result = me->CastSpell(target, spell->Info->Id, false);
    ForeverBot* data = GetBotData();
    if (data)
    {
        data->LastCastSpell = spell->Info->Id;
        data->LastCastResult = int32(result);
    }

    if (result != SPELL_CAST_OK)
    {
        if (data)
        {
            ++data->CastsFailed;
            // why the server refused it (once per spell and reason every 10 s)
            if (data->Debug || sForeverBotMgr->IsGlobalDebug())
            {
                uint32 const now = GameTime::GetGameTimeMS();
                uint64 const key = (uint64(spell->Info->Id) << 16) | uint32(result);
                if (now >= _failLogged[key])
                {
                    _failLogged[key] = now + 10000;
                    TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} cast {} ({}) on {} refused: {} (dist {:.1f}, melee {}, facing {})",
                        me->GetName(), spell->Name, spell->Info->Id, target->GetName(), uint32(result), me->GetDistance(target),
                        me->IsWithinMeleeRange(target), me->HasInArc(float(M_PI) / 2, target));
                }
            }
        }
        // the server's line of sight is stricter than ours: get closer to that target for a while
        if (result == SPELL_FAILED_LINE_OF_SIGHT)
        {
            _losBlocked = target->GetGUID();
            _losBlockedUntil = GameTime::GetGameTimeMS() + 4000;
        }
        // range, facing and line of sight are fixed by moving; anything else blocks the spell for a moment
        if (result != SPELL_FAILED_OUT_OF_RANGE && result != SPELL_FAILED_LINE_OF_SIGHT && result != SPELL_FAILED_UNIT_NOT_INFRONT
            && result != SPELL_FAILED_NOT_READY && result != SPELL_FAILED_SPELL_IN_PROGRESS)
            _blockedSpells[spell->Info->Id] = GameTime::GetGameTimeMS() + RefusedCastGrace;
        return false;
    }

    if (data)
    {
        ++data->CastsOk;
        if (spell->Has(KIND_INTERRUPT) && targetWasCasting)
            ++data->Interrupts;
        if (spell->Has(KIND_TAUNT))
            ++data->Taunts;
        if (spell->Has(KIND_CC))
            ++data->CrowdControls;
        if (spell->Has(KIND_DISPEL) && target->IsFriendlyTo(me))
            ++data->Dispels;
        if (spell->Has(KIND_RESURRECT))
            ++data->Resurrections;
    }
    return true;
}

bool ForeverBotAI::CastAt(BotSpell const* spell, Position const& pos)
{
    if (!CanCast(spell, me, false))
        return false;
    if (!me->IsWithinDist3d(&pos, spell->Info->GetMaxRange(false, me)))
        return false;

    me->SetFacingTo(me->GetAbsoluteAngle(&pos));
    if (spell->CastTime > 0 && me->isMoving())
    {
        me->GetMotionMaster()->Clear();
        me->StopMoving();
    }

    SpellCastResult result = me->CastSpell(pos, spell->Info->Id, false);
    if (ForeverBot* data = GetBotData())
    {
        data->LastCastSpell = spell->Info->Id;
        data->LastCastResult = int32(result);
        ++(result == SPELL_CAST_OK ? data->CastsOk : data->CastsFailed);
    }
    if (result != SPELL_CAST_OK && result != SPELL_FAILED_OUT_OF_RANGE && result != SPELL_FAILED_LINE_OF_SIGHT)
        _blockedSpells[spell->Info->Id] = GameTime::GetGameTimeMS() + RefusedCastGrace;
    return result == SPELL_CAST_OK;
}

bool ForeverBotAI::CastNamed(std::string_view name, Unit* target)
{
    return Cast(_spells.Find(name), target);
}

// any rank of that spell (by name), from me or from anyone
bool ForeverBotAI::HasAuraNamed(Unit const* target, std::string const& name, bool mine) const
{
    for (auto const& [auraId, application] : target->GetAppliedAuras())
    {
        Aura const* aura = application->GetBase();
        if (mine && aura->GetCasterGUID() != me->GetGUID())
            continue;
        SpellInfo const* info = aura->GetSpellInfo();
        if (info->SpellName && name == (*info->SpellName)[LOCALE_enUS])
            return true;
    }
    return false;
}

bool ForeverBotAI::HasAuraFrom(Unit const* target, BotSpell const* spell, bool mine) const
{
    return spell && HasAuraNamed(target, spell->Name, mine);
}

void ForeverBotAI::EnsureAttacking(Unit* target, bool melee)
{
    if (me->GetVictim() != target || melee != me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
    {
        bool attacked = me->Attack(target, melee);
        if (ForeverBot* data = GetBotData())
        {
            data->LastTarget = target->GetName();
            data->LastAttackOk = attacked;
        }
    }
}
