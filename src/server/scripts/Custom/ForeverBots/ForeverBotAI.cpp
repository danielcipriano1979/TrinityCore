/*
 * Forever bots: the AI controller, run from Player::Update (map thread) through the PlayerAI hook.
 *
 * Each tick: build the context (members, enemies, tank, leader, party blackboard), pick the state, let the layers
 * propose candidates, run the most relevant one that can run now, then move. See AI/BotAI.h and
 * docs/BOT_ARCHITECTURE.md.
 */

#include "AI/BotAI.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"

using namespace ForeverBots;

namespace
{
    constexpr uint32 SlowTickInterval = 2000;
}

ForeverBotAI::ForeverBotAI(Player* player) : PlayerAI(player)
{
    // a little variation between bots: they don't all react on the same frame
    _thinkInterval = urand(250, 400);
}

ForeverBot* ForeverBotAI::GetBotData() const
{
    return sForeverBotMgr->GetBot(me->GetGUID());
}

bool ForeverBotAI::BuildContext(BotContext& ctx)
{
    ctx.Me = me;
    ctx.Data = GetBotData();
    if (!ctx.Data)
        return false;
    ctx.Now = GameTime::GetGameTimeMS();
    ctx.AutonomousLeader = ctx.Data->Leader;

    Group* group = me->GetGroup();
    ObjectGuid const key = group ? group->GetGUID() : (ctx.Data->Owner.IsEmpty() ? me->GetGUID() : ctx.Data->Owner);
    ctx.Party = &PartyRegistry::Instance().For(key, ctx.Now);

    Player* owner = ctx.Data->Owner.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*me, ctx.Data->Owner);
    ctx.Leader = owner ? owner : (ctx.AutonomousLeader ? me : nullptr);

    if (group)
    {
        for (GroupReference const& ref : group->GetMembers())
        {
            Player* member = ref.GetSource();
            if (!member || !member->IsInMap(me) || !member->IsInWorld())
                continue;
            if (member->IsAlive())
                ctx.Members.push_back(member);
            else if (!member->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                ctx.Dead.push_back(member);
        }
    }
    else
    {
        ctx.Members.push_back(me);
        if (owner && owner->IsAlive())
            ctx.Members.push_back(owner);
    }

    for (Player* member : ctx.Members)
        if (ctx.RoleOf(member) == BotRole::Tank)
        {
            ctx.Tank = member;
            break;
        }
    ctx.Party->Tank = ctx.Tank ? ctx.Tank->GetGUID() : ObjectGuid::Empty;
    ctx.Party->Leader = ctx.Leader ? ctx.Leader->GetGUID() : ObjectGuid::Empty;

    // everything fighting a member: attackers and the units that have a member on their threat list
    auto addEnemy = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInMap(me) || !me->IsValidAttackTarget(unit))
            return;
        if (std::find(ctx.Enemies.begin(), ctx.Enemies.end(), unit) != ctx.Enemies.end())
            return;
        ctx.Enemies.push_back(unit);
        ctx.Party->EngagedAt.try_emplace(unit->GetGUID(), ctx.Now);
    };
    for (Player* member : ctx.Members)
    {
        for (Unit* attacker : member->getAttackers())
            addEnemy(attacker);
        for (auto const& [guid, ref] : member->GetThreatManager().GetThreatenedByMeList())
            addEnemy(ref->GetOwner());
    }
    ctx.GroupInCombat = !ctx.Enemies.empty();
    if (ctx.GroupInCombat && !ctx.Party->CombatSince)
        ctx.Party->CombatSince = ctx.Now;
    else if (!ctx.GroupInCombat)
        ctx.Party->CombatSince = 0;
    return true;
}

void ForeverBotAI::UpdateDead(BotContext& ctx)
{
    // nobody answers a bot's resurrection request: accept it here
    if (me->IsResurrectRequested())
        me->ResurrectUsingRequestData();
    ctx.Data->State = BotStateName(BotState::Dead);
}

void ForeverBotAI::UpdateAI(uint32 diff)
{
    if (_thinkTimer > diff)
    {
        _thinkTimer -= diff;
        return;
    }
    uint32 const elapsed = _thinkInterval - std::min(_thinkTimer, _thinkInterval) + diff;
    _thinkTimer = _thinkInterval;

    ForeverBot* data = GetBotData();
    if (!data || !data->Ready)
        return;

    BotContext ctx;
    if (!BuildContext(ctx))
        return;

    if (!me->IsAlive())
    {
        _state = BotState::Dead;
        UpdateDead(ctx);
        return;
    }

    // slow tick: the spell book, stale claims, unreachable marks
    _slowTimer += elapsed;
    if (_slowTimer >= SlowTickInterval || _spells.NeedsRebuild(me))
    {
        if (_spells.NeedsRebuild(me))
            _spells.Build(me);
        _slowTimer = 0;
        ctx.Party->Prune(ctx.Now);
        std::erase_if(_unreachable, [&](auto const& entry) { return entry.second <= ctx.Now; });
        std::erase_if(_blockedSpells, [&](auto const& entry) { return entry.second <= ctx.Now; });
    }

    Decide(ctx, elapsed);
}

void ForeverBotAI::Decide(BotContext& ctx, uint32 elapsed)
{
    ForeverBot& data = *ctx.Data;
    BotRole const role = EffectiveRole(ctx);
    _brain.Clear();

    bool const inCombat = ctx.GroupInCombat || me->IsInCombat();
    Unit* target = nullptr;

    if (inCombat)
    {
        _state = BotState::Combat;
        _outOfCombatTime = 0;
        data.CombatTime += elapsed;
        if (me->GetStandState() != UNIT_STAND_STATE_STAND)
            me->SetStandState(UNIT_STAND_STATE_STAND);

        target = role == BotRole::Tank ? SelectTankTarget(ctx) : SelectTarget(ctx);

        ProposeDefensives(ctx);
        ProposeInterrupts(ctx);
        ProposeDispels(ctx);
        if (role == BotRole::Heal || data.IsWorldBot())
            ProposeHealer(ctx);
        else if (!std::any_of(ctx.Members.begin(), ctx.Members.end(), [&](Player* m) { return m != me && ctx.IsHealer(m); }))
            ProposeHealer(ctx, true);       // no healer around: hybrids save whoever is dying

        if (target)
        {
            bool const melee = !IsCasterClass() && (!IsRangedClass() || me->IsWithinMeleeRange(target));
            if (role != BotRole::Heal)
                EnsureAttacking(target, melee);
            if (role == BotRole::Tank)
                ProposeTank(ctx, target);
            else if (role == BotRole::Heal)
            {
                // a healer keeps its mana for heals: it helps with the wand only (no mana)
                if (!me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                    if (BotSpell const* wand = _spells.Find("Shoot"); wand && CanCast(wand, target))
                        _brain.Propose(R_ROTATION - 5, wand->Name.c_str(), target, [this, wand, target]() { return Cast(wand, target); }, "healer, wand");
            }
            else
                ProposeDps(ctx, target);
            ProposeCrowdControl(ctx, target);
        }
        ProposeClassUpkeep(ctx, target, true);
    }
    else
    {
        if (me->GetVictim())
            me->AttackStop();
        if (data.Order == BotOrder::Attack)
            data.Order = BotOrder::Follow;

        // a world bot looks for something of its level to hunt once it is rested
        if (data.IsWorldBot() && !data.Passive && me->GetHealthPct() > 70.0f
            && (me->GetMaxPower(POWER_MANA) == 0 || me->GetPowerPct(POWER_MANA) > 40.0f))
            if (Unit* prey = FindHuntTarget())
            {
                target = prey;
                EnsureAttacking(prey, !IsCasterClass() && !IsRangedClass());
                ProposeDps(ctx, prey);
            }

        ProposeResurrect(ctx);
        ProposeHealer(ctx);
        ProposeBuffs(ctx);
        ProposeClassUpkeep(ctx, target, false);
        ProposePull(ctx);
    }

    size_t const proposals = _brain.Size();
    ForeverBots::Candidate const* done = _brain.Execute();

    // movement runs beside the decisions (not while casting)
    if (inCombat || target)
        MoveInCombat(ctx, target);
    else
    {
        if (!me->HasUnitState(UNIT_STATE_CASTING))
            MoveOutOfCombat(ctx);       // keep up with the group, also between buffs
        bool const resting = me->GetHealthPct() < 95.0f || (me->GetMaxPower(POWER_MANA) && me->GetPowerPct(POWER_MANA) < 95.0f);
        _state = resting ? BotState::Rest : BotState::Idle;
        Rest(ctx, elapsed);         // eats and drinks whenever it stands still out of combat
    }

    data.State = BotStateName(_state);
    (void)proposals;
    LogDecision(ctx, done);
}

void ForeverBotAI::LogDecision(BotContext const& ctx, ForeverBots::Candidate const* done)
{
    ForeverBot& data = *ctx.Data;
    std::string decision = done ? Trinity::StringFormat("{} -> {} ({})", done->Name, done->Target ? done->Target->GetName() : std::string("-"), done->Reason)
        : std::string("none");
    data.Decision = decision;

    if (!data.Debug && !sForeverBotMgr->IsGlobalDebug())
        return;
    // log changes, and a heartbeat every few seconds
    if (decision == _lastDecision && ctx.Now - _lastDecisionLog < 3000)
        return;
    _lastDecision = decision;
    _lastDecisionLog = ctx.Now;

    float const mana = me->GetMaxPower(POWER_MANA) ? me->GetPowerPct(POWER_MANA) : 0.0f;
    TC_LOG_INFO("server.foreverbots", "[BotAI] T={} Bot={} Role={} State={} HP={:.0f}% Mana={:.0f}% Enemies={} Decision={}",
        ctx.Now / 1000, me->GetName(), BotRoleName(data.Role), BotStateName(_state), me->GetHealthPct(), mana, ctx.Enemies.size(), decision);
}

void ForeverBotAI::DamageDealt(Unit* /*victim*/, uint32& damage, DamageEffectType /*damageType*/)
{
    if (ForeverBot* data = GetBotData())
        data->DamageDone += damage;
}

void ForeverBotAI::HealDone(Unit* to, uint32& addhealth)
{
    if (ForeverBot* data = GetBotData())
    {
        uint32 const missing = to->GetMaxHealth() - to->GetHealth();
        data->HealingDone += std::min(addhealth, missing);
        if (addhealth > missing)
            data->Overhealing += addhealth - missing;
    }
}
