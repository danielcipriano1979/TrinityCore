/*
 * Forever bots: target selection, threat, and the role layers (tank, healer, damage) plus the utility layer
 * (interrupts, dispels, crowd control, defensives, buffs, resurrection). Every layer only *proposes* candidates.
 */

#include "BotAI.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"

using namespace ForeverBots;

namespace
{
    constexpr float HealRange = 38.0f;
    constexpr uint32 TankOpenerTime = 3000;         // DPS give the tank that long on a fresh mob
    constexpr float ThreatHoldPct = 90.0f;

    // mechanics that make a debuff worth dispelling first
    constexpr uint64 DangerousMechanics = (UI64LIT(1) << MECHANIC_STUN) | (UI64LIT(1) << MECHANIC_FEAR) | (UI64LIT(1) << MECHANIC_POLYMORPH)
        | (UI64LIT(1) << MECHANIC_CHARM) | (UI64LIT(1) << MECHANIC_SLEEP) | (UI64LIT(1) << MECHANIC_SILENCE) | (UI64LIT(1) << MECHANIC_HORROR)
        | (UI64LIT(1) << MECHANIC_DISORIENTED) | (UI64LIT(1) << MECHANIC_ROOT);
}

BotRole ForeverBotAI::EffectiveRole(BotContext const& ctx) const
{
    // a world bot fights on its own: it heals itself but is a damage dealer
    return ctx.Data->IsWorldBot() ? BotRole::Dps : ctx.Data->Role;
}

bool ForeverBotAI::IsValidEnemy(Unit* unit) const
{
    if (!unit || !unit->IsAlive() || !unit->IsInMap(me) || !me->IsValidAttackTarget(unit))
        return false;
    auto itr = _unreachable.find(unit->GetGUID());
    return itr == _unreachable.end() || itr->second <= GameTime::GetGameTimeMS();
}

uint32 ForeverBotAI::CountEnemiesNear(BotContext const& ctx, Unit const* center, float radius) const
{
    uint32 count = 0;
    for (Unit* enemy : ctx.Enemies)
        if (enemy->IsWithinDist(center, radius))
            ++count;
    return count;
}

float ForeverBotAI::ThreatPctOnTarget(BotContext const& ctx, Unit* target) const
{
    if (!target->CanHaveThreatList())
        return 0.0f;
    ThreatManager& threat = target->GetThreatManager();
    float const mine = threat.GetThreat(me);
    if (mine <= 0.0f)
        return 0.0f;
    float reference = 0.0f;
    if (ctx.Tank && ctx.Tank != me)
        reference = threat.GetThreat(ctx.Tank);
    if (reference <= 0.0f)
        if (Unit* victim = threat.GetCurrentVictim(); victim && victim != me)
            reference = threat.GetThreat(victim);
    return reference > 0.0f ? mine * 100.0f / reference : 200.0f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------------------------------------
Unit* ForeverBotAI::SelectTarget(BotContext& ctx)
{
    if (ctx.Data->Passive)
        return nullptr;

    // a direct order from the owner: their selection, which becomes the group's focus
    if (ctx.Data->Order == BotOrder::Attack)
        if (Player* owner = ObjectAccessor::GetPlayer(*me, ctx.Data->Owner))
            if (Unit* selected = owner->GetSelectedUnit(); IsValidEnemy(selected))
            {
                ctx.Party->FocusTarget = selected->GetGUID();
                ctx.Party->FocusExpires = ctx.Now + 30000;
                return selected;
            }

    Unit* best = nullptr;
    float bestScore = -100000.0f;
    for (Unit* enemy : ctx.Enemies)
    {
        if (!IsValidEnemy(enemy))
            continue;

        float score = 0.0f;
        Unit* victim = enemy->GetVictim();
        if (enemy->GetGUID() == ctx.Party->FocusTarget)
            score += 100.0f;
        if (ctx.Tank && ctx.Tank != me && ctx.Tank->GetVictim() == enemy)
            score += 60.0f;                                         // assist the tank
        if (victim && victim->IsPlayer() && victim != ctx.Tank && ctx.IsHealer(victim->ToPlayer()))
            score += 35.0f;                                         // peel for the healer
        if (victim == me)
            score += 25.0f;
        if (enemy == me->GetVictim())
            score += 20.0f;                                         // don't hop between targets
        if (enemy->HasBreakableByDamageCrowdControlAura() || ctx.Party->IsClaimedByOther(ctx.Party->CrowdControl, enemy->GetGUID(), me->GetGUID(), ctx.Now))
            score -= 500.0f;                                        // never break crowd control
        if (Creature* creature = enemy->ToCreature(); creature && creature->IsDungeonBoss())
            score += 5.0f;
        score += (100.0f - enemy->GetHealthPct()) * 0.15f;          // finish what is almost dead
        score -= me->GetDistance(enemy);

        if (score > bestScore)
        {
            bestScore = score;
            best = enemy;
        }
    }
    return best;
}

Unit* ForeverBotAI::SelectTankTarget(BotContext& ctx)
{
    if (ctx.Data->Passive)
        return nullptr;

    // a mob hitting someone else: the healer first, then the others
    Unit* loose = nullptr;
    bool looseOnHealer = false;
    for (Unit* enemy : ctx.Enemies)
    {
        if (!IsValidEnemy(enemy) || enemy->HasBreakableByDamageCrowdControlAura())
            continue;
        Unit* victim = enemy->GetVictim();
        if (!victim || victim == me || !victim->IsPlayer())
            continue;
        // raids have several tanks: a mob on another tank is held, not loose
        if (ctx.RoleOf(victim->ToPlayer()) == BotRole::Tank)
            continue;
        bool const onHealer = ctx.IsHealer(victim->ToPlayer());
        if (!loose || (onHealer && !looseOnHealer) || (onHealer == looseOnHealer && me->GetDistance(enemy) < me->GetDistance(loose)))
        {
            loose = enemy;
            looseOnHealer = onHealer;
        }
    }
    if (loose)
        return loose;

    if (Unit* victim = me->GetVictim(); IsValidEnemy(victim) && !victim->HasBreakableByDamageCrowdControlAura())
        return victim;

    // the one on me where my lead is the smallest, else the nearest
    Unit* best = nullptr;
    float bestLead = 100000.0f;
    for (Unit* enemy : ctx.Enemies)
    {
        if (!IsValidEnemy(enemy) || enemy->HasBreakableByDamageCrowdControlAura())
            continue;
        float lead = enemy->CanHaveThreatList() ? enemy->GetThreatManager().GetThreat(me) : 0.0f;
        lead += me->GetDistance(enemy) * 10.0f;
        if (lead < bestLead)
        {
            bestLead = lead;
            best = enemy;
        }
    }
    if (best)
        return best;

    if (Unit* pull = ObjectAccessor::GetUnit(*me, ctx.Party->PullTarget); IsValidEnemy(pull))
        return pull;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tank
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeTank(BotContext& ctx, Unit* target)
{
    // the main tank is the first tank of the group (BuildContext); a lone tank that the group does not list is it
    if (!ctx.Tank)
        ctx.Party->Tank = me->GetGUID();

    // taunt what got loose: on a player that is not a tank (raids have several, never taunt off each other)
    auto isLoose = [&](Unit const* victim)
    {
        return victim && victim != me && victim->IsPlayer() && ctx.RoleOf(victim->ToPlayer()) != BotRole::Tank;
    };
    uint32 loose = 0;
    for (Unit* enemy : ctx.Enemies)
        if (isLoose(enemy->GetVictim()))
            ++loose;

    Unit* victim = target->GetVictim();
    if (isLoose(victim))
    {
        for (BotSpell const* taunt : _spells.Of(KIND_TAUNT))
        {
            bool const area = taunt->Info->IsAffectingArea();
            if (area && loose < 3)
                continue;
            Unit* castTarget = area ? me : target;
            if (!CanCast(taunt, castTarget))
                continue;
            _brain.Propose(R_TAUNT + (area ? 2 : 0), taunt->Name.c_str(), target, [this, taunt, castTarget]() { return Cast(taunt, castTarget); },
                Trinity::StringFormat("{} is hitting {}", target->GetName(), victim->GetName()));
        }
    }

    // threat on everything around when several mobs are up: the class layer knows the area threat spells
    ProposeClassRotation(ctx, target, 0);
    ProposeGenericRotation(ctx, target, -5);
}

// ---------------------------------------------------------------------------------------------------------------------
// Healer
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeHealer(BotContext& ctx, bool emergencyOnly /*= false*/)
{
    std::vector<BotSpell const*> heals = _spells.Of(KIND_DIRECT_HEAL | KIND_HOT | KIND_SHIELD | KIND_AOE_HEAL);
    if (heals.empty())
        return;

    float const manaPct = me->GetPowerType() == POWER_MANA || me->GetMaxPower(POWER_MANA) ? me->GetPowerPct(POWER_MANA) : 100.0f;

    // who: health + distance, worse when out of reach or already being healed; the tank wins ties
    Player* best = nullptr;
    float bestScore = 1000.0f;
    uint32 hurt = 0;
    for (Player* member : ctx.Members)
    {
        float const pct = member->GetHealthPct();
        if (pct < 75.0f && me->IsWithinDistInMap(member, 30.0f))
            ++hurt;
        if (pct >= 97.0f)
            continue;
        int32 const incoming = ctx.Party->IncomingHeal(member->GetGUID(), ctx.Now);
        float const effective = pct + float(incoming) * 100.0f / float(std::max<uint32>(1, member->GetMaxHealth()));
        if (effective >= 90.0f)
            continue;
        float score = effective + me->GetDistance(member) / 10.0f;
        if (!me->IsWithinDistInMap(member, HealRange) || !me->IsWithinLOSInMap(member, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2))
            score += 30.0f;
        if (incoming > 0)
            score += 10.0f;
        if (ctx.RoleOf(member) == BotRole::Tank)
            score -= 3.0f;
        if (score < bestScore)
        {
            bestScore = score;
            best = member;
        }
    }

    // group damage: an area heal
    if (hurt >= 3)
        for (BotSpell const* spell : heals)
            if (spell->Has(KIND_AOE_HEAL) && CanCast(spell, me, false))
                _brain.Propose(R_HEAL + 8, spell->Name.c_str(), me, [this, spell]() { return Cast(spell, me); },
                    Trinity::StringFormat("{} members under 75%", hurt));

    if (!best)
        return;

    float const pct = best->GetHealthPct();
    bool const critical = pct < 35.0f;
    if (emergencyOnly && !critical)
        return;
    // mana: the lower it gets, the more hurt someone must be
    if ((manaPct < 20.0f && !critical) || (manaPct < 40.0f && pct > 60.0f))
        return;

    int32 const missing = int32(best->GetMaxHealth() - best->GetHealth()) - ctx.Party->IncomingHeal(best->GetGUID(), ctx.Now);
    int32 const base = critical ? R_CRITICAL_HEAL + int32(35.0f - pct) : R_HEAL + int32((100.0f - pct) / 10.0f);
    std::string const why = Trinity::StringFormat("{} at {:.0f}% (missing {})", best->GetName(), pct, missing);

    // a shield or a HoT first when it fits; then the direct heal that wastes the least
    BotSpell const* bestDirect = nullptr;
    BotSpell const* fastest = nullptr;
    for (BotSpell const* spell : heals)
    {
        if (!CanCast(spell, best))
            continue;

        if (spell->Has(KIND_SHIELD) && best->IsInCombat() && (critical || pct < 70.0f || (ctx.RoleOf(best) == BotRole::Tank && pct < 85.0f)) && !HasAuraFrom(best, spell, false))
            _brain.Propose(base + 3, spell->Name.c_str(), best, [this, spell, best]() { return Cast(spell, best); }, why);
        else if (spell->Has(KIND_HOT) && !spell->Has(KIND_DIRECT_HEAL) && missing > int32(best->GetMaxHealth() * 15 / 100) && !HasAuraFrom(best, spell, true))
            _brain.Propose(base + (critical ? -5 : 1), spell->Name.c_str(), best, [this, spell, best, &ctx]()
            {
                if (!Cast(spell, best))
                    return false;
                ctx.Party->AddHeal(best->GetGUID(), me->GetGUID(), spell->Amount / 2, ctx.Now + 6000);
                return true;
            }, why);
        else if (spell->Has(KIND_DIRECT_HEAL))
        {
            if (!fastest || spell->CastTime < fastest->CastTime || (spell->CastTime == fastest->CastTime && spell->Amount > fastest->Amount))
                fastest = spell;
            // efficiency: the biggest heal that doesn't overheal much, else the smallest one
            bool const fits = spell->Amount <= int32(float(missing) * 1.3f);
            if (!bestDirect)
                bestDirect = spell;
            else
            {
                bool const bestFits = bestDirect->Amount <= int32(float(missing) * 1.3f);
                if ((fits && !bestFits) || (fits && bestFits && spell->Amount > bestDirect->Amount) || (!fits && !bestFits && spell->Amount < bestDirect->Amount))
                    bestDirect = spell;
            }
        }
    }

    // debug: someone needs a heal and no heal can be cast on them (why a healer stands there with full mana)
    if (!bestDirect && _brain.Size() == 0 && (ctx.Data->Debug || sForeverBotMgr->IsGlobalDebug()) && ctx.Now >= _healBlockedLog)
    {
        _healBlockedLog = ctx.Now + 3000;
        TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} can't heal {} at {:.0f}%: dist {:.1f}, line of sight {}, casting {}, moving {}, {} heals known",
            me->GetName(), best->GetName(), pct, me->GetDistance(best), me->IsWithinLOSInMap(best, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2), me->HasUnitState(UNIT_STATE_CASTING),
            me->isMoving(), heals.size());
    }

    BotSpell const* direct = critical && fastest ? fastest : bestDirect;
    // not worth a direct heal yet: less missing than most of the smallest heal would restore
    if (direct && !critical && missing < int32(float(direct->Amount) * 0.7f) && pct > 70.0f)
        direct = nullptr;
    if (direct)
        _brain.Propose(base, direct->Name.c_str(), best, [this, direct, best, &ctx]()
        {
            if (!Cast(direct, best))
                return false;
            ctx.Party->AddHeal(best->GetGUID(), me->GetGUID(), direct->Amount, ctx.Now + direct->CastTime + 500);
            return true;
        }, why);
}

// ---------------------------------------------------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeDps(BotContext& ctx, Unit* target, int32 relevanceOffset /*= 0*/)
{
    bool const tankLeads = ctx.Tank && ctx.Tank != me && ctx.Tank->IsAlive();

    if (tankLeads && EffectiveRole(ctx) != BotRole::Tank)
    {
        // give the tank a moment on a fresh mob, unless it is already on one of us
        Unit* victim = target->GetVictim();
        auto engaged = ctx.Party->EngagedAt.find(target->GetGUID());
        bool const fresh = engaged != ctx.Party->EngagedAt.end() && ctx.Now - engaged->second < TankOpenerTime;
        // ...whoever it is running at: hitting a fresh mob that is not on the tank yet is what puts it on a damage dealer
        // for good. Only the one it is already beating on defends itself, and nobody waits for a tank that is far away.
        if (fresh && victim != me && me->IsWithinDistInMap(ctx.Tank, 40.0f))
        {
            _brain.Propose(R_ROTATION + 25, "WAIT_TANK", target, []() { return true; }, "tank opener");
            return;
        }

        // too close to the tank's threat: hold the big hits
        float const threat = ThreatPctOnTarget(ctx, target);
        if (threat >= ThreatHoldPct && victim == ctx.Tank)
        {
            _brain.Propose(R_ROTATION + 25, "HOLD_THREAT", target, []() { return true; }, Trinity::StringFormat("threat {:.0f}% of the tank", threat));
            return;
        }
    }

    ProposeClassRotation(ctx, target, relevanceOffset);
    ProposeGenericRotation(ctx, target, relevanceOffset - 5);
}

// ---------------------------------------------------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeInterrupts(BotContext& ctx)
{
    std::vector<BotSpell const*> interrupts = _spells.Of(KIND_INTERRUPT);
    if (interrupts.empty())
        return;

    for (Unit* enemy : ctx.Enemies)
    {
        Spell const* casting = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!casting)
            casting = enemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!casting || !casting->GetSpellInfo()->CanBeInterrupted(me, enemy))
            continue;
        // one bot per cast: the others stay as backup until the claim expires
        if (ctx.Party->IsClaimedByOther(ctx.Party->Interrupts, enemy->GetGUID(), me->GetGUID(), ctx.Now))
            continue;

        for (BotSpell const* spell : interrupts)
        {
            if (!CanCast(spell, enemy))
                continue;
            Creature* creature = enemy->ToCreature();
            int32 const relevance = R_INTERRUPT + (creature && (creature->IsDungeonBoss() || creature->IsElite()) ? 5 : 0);
            PartyBlackboard* party = ctx.Party;
            uint32 const now = ctx.Now;
            _brain.Propose(relevance, spell->Name.c_str(), enemy, [this, spell, enemy, party, now]()
            {
                if (!party->TryClaim(party->Interrupts, enemy->GetGUID(), me->GetGUID(), now, 1500))
                    return false;
                return Cast(spell, enemy);
            }, Trinity::StringFormat("{} casts {}", enemy->GetName(), (*casting->GetSpellInfo()->SpellName)[LOCALE_enUS]));
            break;
        }
    }
}

void ForeverBotAI::ProposeDispels(BotContext& ctx)
{
    std::vector<BotSpell const*> dispels = _spells.Of(KIND_DISPEL);
    if (dispels.empty())
        return;

    bool const healer = EffectiveRole(ctx) == BotRole::Heal;
    for (Player* member : ctx.Members)
    {
        for (BotSpell const* spell : dispels)
        {
            DispelChargesList list;
            member->GetDispellableAuraList(me, spell->DispelMask, list);
            if (list.empty() || !CanCast(spell, member))
                continue;

            bool dangerous = false;
            std::string auraName;
            for (DispelableAura const& dispelable : list)
            {
                SpellInfo const* info = dispelable.GetAura()->GetSpellInfo();
                if (info->GetAllEffectsMechanicMask() & DangerousMechanics)
                    dangerous = true;
                if (auraName.empty() && info->SpellName)
                    auraName = (*info->SpellName)[LOCALE_enUS];
            }
            int32 relevance = (healer ? R_DISPEL : R_DISPEL - 15) + (dangerous ? 15 : 0);
            if (ctx.RoleOf(member) == BotRole::Tank)
                relevance += 3;
            _brain.Propose(relevance, spell->Name.c_str(), member, [this, spell, member]() { return Cast(spell, member); },
                Trinity::StringFormat("{} has {}", member->GetName(), auraName));
            break;
        }
    }
}

void ForeverBotAI::ProposeDefensives(BotContext& ctx)
{
    float const pct = me->GetHealthPct();

    // pulled aggro off the tank: drop threat
    if (ctx.Tank && ctx.Tank != me && ctx.Tank->IsAlive())
        for (Unit* enemy : ctx.Enemies)
            if (enemy->GetVictim() == me)
            {
                for (BotSpell const* spell : _spells.Of(KIND_THREAT_DROP))
                    if (CanCast(spell, spell->Info->NeedsExplicitUnitTarget() ? enemy : me))
                    {
                        Unit* castTarget = spell->Info->NeedsExplicitUnitTarget() ? enemy : me;
                        _brain.Propose(R_EMERGENCY - 20, spell->Name.c_str(), castTarget, [this, spell, castTarget]() { return Cast(spell, castTarget); },
                            Trinity::StringFormat("{} is on me", enemy->GetName()));
                    }
                break;
            }

    if (pct >= 35.0f)
        return;
    for (BotSpell const* spell : _spells.Of(KIND_DEFENSIVE))
        if (CanCast(spell, me))
            _brain.Propose(R_EMERGENCY, spell->Name.c_str(), me, [this, spell]() { return Cast(spell, me); },
                Trinity::StringFormat("health {:.0f}%", pct));
}

void ForeverBotAI::ProposeCrowdControl(BotContext& ctx, Unit* target)
{
    // only with a real fight going: three or more mobs, someone tanking, and I'm not the tank or the healer
    BotRole const role = EffectiveRole(ctx);
    if (role != BotRole::Dps || ctx.Enemies.size() < 3 || !ctx.Tank || ctx.Tank == me)
        return;

    std::vector<BotSpell const*> controls = _spells.Of(KIND_CC);
    if (controls.empty())
        return;

    for (Unit* enemy : ctx.Enemies)
    {
        if (enemy == target || enemy == ctx.Tank->GetVictim() || enemy->GetGUID() == ctx.Party->FocusTarget)
            continue;
        if (enemy->HasBreakableByDamageCrowdControlAura() || ctx.Party->CrowdControl.contains(enemy->GetGUID()))
            continue;
        Creature* creature = enemy->ToCreature();
        if (creature && creature->IsDungeonBoss())
            continue;

        for (BotSpell const* spell : controls)
        {
            // fear sends mobs running into other packs: not in a group
            if (spell->Info->HasAura(SPELL_AURA_MOD_FEAR) || spell->Info->IsAffectingArea() || !CanCast(spell, enemy))
                continue;
            PartyBlackboard* party = ctx.Party;
            uint32 const now = ctx.Now;
            uint32 const duration = uint32(std::max(spell->Info->GetMaxDuration(), 5000));
            _brain.Propose(R_CC, spell->Name.c_str(), enemy, [this, spell, enemy, party, now, duration]()
            {
                if (!party->TryClaim(party->CrowdControl, enemy->GetGUID(), me->GetGUID(), now, duration))
                    return false;
                return Cast(spell, enemy);
            }, Trinity::StringFormat("{} mobs, {} is free", ctx.Enemies.size(), enemy->GetName()));
            return;     // one at a time
        }
    }
}

void ForeverBotAI::ProposeBuffs(BotContext& ctx)
{
    for (BotSpell const* spell : _spells.Of(KIND_SELF_BUFF | KIND_BUFF))
    {
        if (spell->Has(KIND_SELF_BUFF))
        {
            if (!HasAuraFrom(me, spell, false) && CanCast(spell, me))
                _brain.Propose(R_BUFF, spell->Name.c_str(), me, [this, spell]() { return Cast(spell, me); }, "self buff missing");
            continue;
        }
        for (Player* member : ctx.Members)
            if (!HasAuraFrom(member, spell, false) && CanCast(spell, member))
            {
                _brain.Propose(R_BUFF - 1, spell->Name.c_str(), member, [this, spell, member]() { return Cast(spell, member); },
                    Trinity::StringFormat("{} has no {}", member->GetName(), spell->Name));
                break;
            }
    }
}

void ForeverBotAI::ProposeResurrect(BotContext& ctx)
{
    std::vector<BotSpell const*> spells = _spells.Of(KIND_RESURRECT);
    if (spells.empty())
        return;
    for (Player* dead : ctx.Dead)
    {
        if (dead->IsResurrectRequested())
            continue;
        for (BotSpell const* spell : spells)
            if (CanCast(spell, dead))
            {
                _brain.Propose(R_HEAL + 15, spell->Name.c_str(), dead, [this, spell, dead]() { return Cast(spell, dead); },
                    Trinity::StringFormat("{} is dead", dead->GetName()));
                return;
            }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// World bots: something of their level to hunt
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
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
            return Me->IsValidAttackTarget(creature) && Me->IsWithinLOSInMap(creature, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
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
        if (IsValidEnemy(creature) && (!nearest || me->GetDistance(creature) < me->GetDistance(nearest)))
            nearest = creature;
    return nearest;
}
