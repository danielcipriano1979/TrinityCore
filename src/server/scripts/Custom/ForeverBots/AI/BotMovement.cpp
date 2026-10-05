/*
 * Forever bots: movement. Combat positions by role (the tank faces the mob, melee goes behind it, ranged and
 * casters keep their range with line of sight, the healer stays in reach of the tank), formation while following,
 * stuck detection, the pull of a bot-only party's tank, and resting between fights.
 *
 * Movement uses the MotionMaster (paths come from the mmaps where they exist); a bot is never teleported in a fight.
 */

#include "BotAI.h"
#include "CellImpl.h"
#include "ChaseMovementGenerator.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"

using namespace ForeverBots;

namespace
{
    constexpr float HealerReach = 28.0f;
    constexpr float PullRange = 45.0f;
    constexpr float ExploreRange = 150.0f;
    constexpr float ReadyDistance = 35.0f;
}

bool ForeverBotAI::IsChasing(Unit* target) const
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
        return false;
    ChaseMovementGenerator const* chase = dynamic_cast<ChaseMovementGenerator const*>(me->GetMotionMaster()->GetCurrentMovementGenerator());
    return chase && chase->GetTarget() == target;
}

void ForeverBotAI::ChaseTarget(Unit* target, float minRange, float maxRange, Optional<float> angle)
{
    if (IsChasing(target))
        return;
    MotionMaster* motion = me->GetMotionMaster();
    motion->Clear();
    Optional<ChaseRange> range;
    if (maxRange > 0.0f)
        range.emplace(minRange, maxRange);
    Optional<ChaseAngle> chaseAngle;
    if (angle)
        chaseAngle.emplace(*angle);
    motion->MoveChase(target, range, chaseAngle);
}

void ForeverBotAI::CheckStuck(Unit* target, float wantedRange, uint32 elapsed)
{
    if (target->GetGUID() != _stuckTarget)
    {
        _stuckTarget = target->GetGUID();
        _stuckLastDist = me->GetDistance(target);
        _stuckTime = 0;
        return;
    }

    float const dist = me->GetDistance(target);
    if (dist <= wantedRange + 1.0f || dist < _stuckLastDist - 0.5f)
    {
        _stuckLastDist = dist;
        _stuckTime = 0;
        return;
    }

    _stuckTime += elapsed;
    if (_stuckTime >= 6000)
    {
        // can't get there: leave it to the others for a while
        _unreachable[target->GetGUID()] = GameTime::GetGameTimeMS() + 8000;
        _stuckTime = 0;
        me->AttackStop();
        me->GetMotionMaster()->Clear();
        if (ForeverBot* data = GetBotData(); data && (data->Debug || sForeverBotMgr->IsGlobalDebug()))
            TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} can't reach {} (dist {:.1f}), skipping it", me->GetName(), target->GetName(), dist);
    }
    else if (_stuckTime >= 3000 && _stuckTime - elapsed < 3000)
        me->GetMotionMaster()->Clear();         // a fresh path next tick
}

void ForeverBotAI::MoveInCombat(BotContext& ctx, Unit* target)
{
    ForeverBot const& data = *ctx.Data;
    if (data.Order == BotOrder::Stay)
    {
        if (me->isMoving())
            me->StopMoving();
        if (target && !me->HasUnitState(UNIT_STATE_CASTING))
            me->SetFacingToObject(target);
        return;
    }
    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    MotionMaster* motion = me->GetMotionMaster();
    BotRole const role = EffectiveRole(ctx);
    uint32 const elapsed = _thinkInterval;

    // the healer: in reach and in sight of the tank (or the leader), out of the melee
    if (role == BotRole::Heal)
    {
        Player* anchor = ctx.Tank && ctx.Tank != me && ctx.Tank->IsAlive() ? ctx.Tank : ctx.Leader;
        if (!anchor || anchor == me)
        {
            if (target && !me->IsWithinDistInMap(target, HealerReach))
                ChaseTarget(target, 15.0f, 25.0f, {});
            return;
        }

        bool attackedInMelee = false;
        for (Unit* enemy : ctx.Enemies)
            if (enemy->GetVictim() == me && me->IsWithinMeleeRange(enemy))
                attackedInMelee = true;

        if (!me->IsWithinDistInMap(anchor, HealerReach) || !me->IsWithinLOSInMap(anchor))
            ChaseTarget(anchor, 10.0f, 20.0f, {});
        else if (attackedInMelee && anchor == ctx.Tank && !me->IsWithinDistInMap(anchor, 5.0f))
            ChaseTarget(anchor, 0.0f, 3.0f, {});           // bring the mob to the tank
        else if (motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE || motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        {
            motion->Clear();
            me->StopMoving();
        }
        return;
    }

    if (!target)
        return;

    float const range = PreferredRange();
    if (range > 0.0f && role != BotRole::Tank)
    {
        float const dist = me->GetDistance(target);
        bool const serverRefusedLos = _losBlocked == target->GetGUID() && GameTime::GetGameTimeMS() < _losBlockedUntil;
        bool const los = !serverRefusedLos && me->IsWithinLOSInMap(target);
        if (serverRefusedLos)
        {
            // walk in until the server sees it too
            ChaseTarget(target, 0.0f, std::max(5.0f, dist * 0.5f), {});
            return;
        }

        // a hunter can't shoot point blank: step out unless the mob is on me
        if (IsRangedClass() && dist < 8.0f && target->GetVictim() && target->GetVictim() != me)
        {
            float const angle = target->GetAbsoluteAngle(me);
            Position away(target->GetPositionX() + 14.0f * std::cos(angle), target->GetPositionY() + 14.0f * std::sin(angle), target->GetPositionZ());
            me->UpdateAllowedPositionZ(away.m_positionX, away.m_positionY, away.m_positionZ);
            motion->Clear();
            motion->MovePoint(0, away);
            return;
        }

        if (dist <= range && los)
        {
            if (motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE || motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            {
                motion->Clear();
                me->StopMoving();
            }
            if (!me->HasInArc(float(M_PI) / 2, target))
                me->SetFacingToObject(target);
            _stuckTime = 0;
            return;
        }
        ChaseTarget(target, range * 0.5f, range - 3.0f, {});
        CheckStuck(target, range, elapsed);
        return;
    }

    // melee: the tank in front, the others behind the mob when it faces someone else
    Optional<float> angle;
    if (role != BotRole::Tank && target->GetVictim() && target->GetVictim() != me)
        angle = float(M_PI);
    if (!IsChasing(target))
        ChaseTarget(target, 0.0f, 0.0f, angle);
    CheckStuck(target, NOMINAL_MELEE_RANGE, elapsed);

    // melee swings need the target in front: a client turns its player, a bot has to be turned
    if (me->IsWithinMeleeRange(target) && !me->isMoving() && !me->HasInArc(float(M_PI) / 2, target))
        me->SetFacingToObject(target);
}

void ForeverBotAI::MoveOutOfCombat(BotContext& ctx)
{
    ForeverBot const& data = *ctx.Data;
    if (data.IsWorldBot())
    {
        Wander(data, _thinkInterval);
        return;
    }
    if (data.Order == BotOrder::Stay || !ctx.Leader || ctx.Leader == me)
        return;

    MotionMaster* motion = me->GetMotionMaster();

    // a follow that doesn't bring me closer (formation spot in a wall, no path): straight to the leader, then regroup
    float const dist = me->GetDistance(ctx.Leader);
    if (dist > 12.0f && dist >= _followLastDist - 0.5f)
        _followStuckTime += _thinkInterval;
    else
        _followStuckTime = 0;
    _followLastDist = dist;
    if (_followStuckTime >= 12000)
    {
        _followStuckTime = 0;
        ctx.Data->SummonRequested = true;       // the manager (world thread) brings it next to its owner
        if (ctx.Data->Debug || sForeverBotMgr->IsGlobalDebug())
            TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} couldn't follow {} ({:.0f} yd), regrouped", me->GetName(), ctx.Leader->GetName(), dist);
        return;
    }
    if (_followStuckTime >= 4000)
    {
        if (motion->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            motion->Clear();
            motion->MovePoint(0, ctx.Leader->GetPosition());
        }
        return;
    }

    if (motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        return;

    // formation around the leader: the tank ahead (unless the tank leads), the healer behind, damage on the flanks
    float angle = float(M_PI);
    float distance = 3.0f;
    switch (EffectiveRole(ctx))
    {
        case BotRole::Tank:
            angle = 0.0f;
            distance = 3.0f;
            break;
        case BotRole::Heal:
            angle = float(M_PI);
            distance = 5.0f;
            break;
        default:
            angle = float(M_PI) + ((data.Index % 2) ? 0.8f : -0.8f) + 0.25f * float(data.Index / 2);
            distance = 3.5f + 0.5f * float(data.Index / 2);
            break;
    }
    motion->Clear();
    motion->MoveFollow(ctx.Leader, distance, ChaseAngle(angle));
}

bool ForeverBotAI::IsPartyReadyToPull(BotContext const& ctx, std::string* reason) const
{
    auto notReady = [reason](std::string why) { if (reason) *reason = std::move(why); return false; };
    if (!ctx.Dead.empty())
        return notReady(Trinity::StringFormat("{} is dead", ctx.Dead.front()->GetName()));
    for (Player* member : ctx.Members)
    {
        if (!member->IsWithinDistInMap(me, ReadyDistance))
            return notReady(Trinity::StringFormat("{} is {:.0f} yd away", member->GetName(), me->GetDistance(member)));
        if (member->GetHealthPct() < 70.0f)
            return notReady(Trinity::StringFormat("{} at {:.0f}% health", member->GetName(), member->GetHealthPct()));
        if (ctx.IsHealer(member) && member->GetMaxPower(POWER_MANA) && member->GetPowerPct(POWER_MANA) < 60.0f)
            return notReady(Trinity::StringFormat("{} at {:.0f}% mana", member->GetName(), member->GetPowerPct(POWER_MANA)));
        if (member->HasUnitState(UNIT_STATE_CASTING))
            return notReady(Trinity::StringFormat("{} is casting", member->GetName()));
    }
    return true;
}

namespace
{
    struct PullCheck
    {
        Player* Me;
        float Range;
        bool operator()(Creature* creature) const
        {
            if (!creature->IsAlive() || !Me->IsWithinDistInMap(creature, Range))
                return false;
            if (creature->IsCritter() || creature->IsCivilian() || creature->IsTotem() || creature->GetCreatureTemplate()->npcflag)
                return false;
            if (creature->IsInCombat())
                return false;
            if (int32(creature->GetLevel()) > int32(Me->GetLevel()) + 4)
                return false;
            return Me->IsValidAttackTarget(creature) && creature->IsHostileTo(Me);
        }
    };
}

// a bot-only party: its tank leads, pulling the nearest pack once everyone is ready
void ForeverBotAI::ProposePull(BotContext& ctx)
{
    if (!ctx.AutonomousLeader || ctx.GroupInCombat || ctx.Data->Passive)
        return;

    std::string why;
    if (!IsPartyReadyToPull(ctx, &why))
    {
        _brain.Propose(R_MOVE, "WAIT_PARTY", nullptr, [this]()
        {
            if (me->isMoving())
            {
                me->GetMotionMaster()->Clear();
                me->StopMoving();
            }
            return true;
        }, why);
        return;
    }

    // candidates: in a dungeon every living hostile of the instance (a small map: the next pack may be far, or the
    // ones around unreachable); elsewhere what is around
    std::list<Creature*> found;
    if (me->GetMap()->IsDungeon())
    {
        PullCheck anywhere{ me, 100000.0f };
        for (auto const& [spawnId, creature] : me->GetMap()->GetCreatureBySpawnIdStore())
            if (creature->IsInWorld() && anywhere(creature))
                found.push_back(creature);
    }
    else
    {
        PullCheck check{ me, ExploreRange };
        Trinity::CreatureListSearcher<PullCheck> searcher(me, found, check);
        Cell::VisitGridObjects(me, searcher, ExploreRange);
    }

    // nearest by path (straight line where there is no navmesh); paths only for the few nearest
    found.sort([this](Creature* a, Creature* b) { return me->GetExactDist2dSq(a) < me->GetExactDist2dSq(b); });

    Creature* best = nullptr;
    float bestLength = 100000.0f;
    uint32 paths = 0;
    for (Creature* creature : found)
    {
        if (!IsValidEnemy(creature))
            continue;
        float length = me->GetDistance(creature);
        if (length >= bestLength || paths >= 8)
            break;              // sorted: a path is never shorter than the straight line
        ++paths;
        PathGenerator path(me);
        // the bot moves in straight lines (see UpdateAI), but reachability needs the real navmesh path
        me->ClearUnitState(UNIT_STATE_IGNORE_PATHFINDING);
        bool const ok = path.CalculatePath(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        me->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);
        // a far target may have a partial path (length limit): fine to walk toward it
        uint32 const bad = length > PullRange ? uint32(PATHFIND_NOPATH) : uint32(PATHFIND_NOPATH | PATHFIND_INCOMPLETE);
        if (!ok || (path.GetPathType() & bad))
        {
            _unreachable[creature->GetGUID()] = GameTime::GetGameTimeMS() + 20000;     // don't ask again for a while
            continue;
        }
        if (!(path.GetPathType() & PATHFIND_NOT_USING_PATH))
            length = path.GetPathLength();
        if (length < bestLength)
        {
            bestLength = length;
            best = creature;
        }
    }
    if (!best)
    {
        if ((ctx.Data->Debug || sForeverBotMgr->IsGlobalDebug()) && ctx.Now - _lastDecisionLog > 3000)
            TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} nothing to pull: {} candidates, {} paths tried, {} marked unreachable",
                me->GetName(), found.size(), paths, _unreachable.size());
        return;
    }

    PartyBlackboard* party = ctx.Party;
    uint32 const now = ctx.Now;
    bool const close = bestLength <= PullRange;
    _brain.Propose(R_ROTATION, close ? "PULL" : "EXPLORE", best, [this, best, party, now, close]()
    {
        if (close)
        {
            party->PullTarget = best->GetGUID();
            party->PullStarted = now;
            party->FocusTarget = best->GetGUID();
            party->FocusExpires = now + 30000;
            EnsureAttacking(best, true);
            ChaseTarget(best, 0.0f, 0.0f, {});
        }
        else if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            // walk toward the next pack (stop short of its aggro range)
            float const angle = best->GetAbsoluteAngle(me);
            Position spot(best->GetPositionX() + 25.0f * std::cos(angle), best->GetPositionY() + 25.0f * std::sin(angle), best->GetPositionZ());
            me->UpdateAllowedPositionZ(spot.m_positionX, spot.m_positionY, spot.m_positionZ);
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MovePoint(0, spot);
        }
        return true;
    }, Trinity::StringFormat("{} at {:.0f} yd", best->GetName(), bestLength));
}

void ForeverBotAI::Rest(BotContext& ctx, uint32 elapsed)
{
    _outOfCombatTime += elapsed;
    if (me->isMoving() || me->HasUnitState(UNIT_STATE_CASTING) || _outOfCombatTime < 3000)
    {
        if (me->GetStandState() == UNIT_STAND_STATE_SIT && me->isMoving())
            me->SetStandState(UNIT_STAND_STATE_STAND);
        return;
    }

    bool const usesMana = me->GetMaxPower(POWER_MANA) > 0;
    bool const needs = me->GetHealthPct() < 95.0f || (usesMana && me->GetPowerPct(POWER_MANA) < 95.0f);
    if (!needs)
    {
        if (me->GetStandState() == UNIT_STAND_STATE_SIT)
            me->SetStandState(UNIT_STAND_STATE_STAND);
        return;
    }

    // eating and drinking: sit and recover about 4% a second
    if (me->GetStandState() != UNIT_STAND_STATE_SIT)
        me->SetStandState(UNIT_STAND_STATE_SIT);
    uint32 const pct = std::max<uint32>(1, elapsed * 4 / 1000);
    if (me->GetHealthPct() < 100.0f)
        me->ModifyHealth(me->CountPctFromMaxHealth(pct));
    if (usesMana && me->GetPowerPct(POWER_MANA) < 100.0f)
        me->ModifyPower(POWER_MANA, int32(me->GetMaxPower(POWER_MANA) * pct / 100));
    (void)ctx;
}

void ForeverBotAI::Wander(ForeverBot const& data, uint32 elapsed)
{
    if (me->isMoving() || me->HasUnitState(UNIT_STATE_CASTING))
        return;
    if (_wanderTimer > elapsed)
    {
        _wanderTimer -= elapsed;
        return;
    }
    _wanderTimer = urand(12000, 30000);
    Position pos = me->GetRandomPoint(data.Home, 30.0f);
    me->GetMotionMaster()->MovePoint(1, pos);
}
