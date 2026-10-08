/*
 * Forever bots: party AI (docs/BOT_ARCHITECTURE.md).
 *
 * Every tick the layers *propose* candidate actions with a relevance (BotBrain); the brain runs the most relevant one
 * that succeeds. Spells come from the bot's spell book, classified from SpellInfo (heals, damage, interrupts,
 * dispels, taunts, crowd control, resurrection, buffs, defensives); the class layer only orders and tunes them.
 * The members of a group share a blackboard (interrupt and crowd control claims, incoming heals, focus, pull).
 *
 * Runs on the map thread (PlayerAI::UpdateAI); it only reads the manager's bot data.
 */

#ifndef FOREVER_BOT_AI_H
#define FOREVER_BOT_AI_H

#include "ModelIgnoreFlags.h"
#include "../ForeverBots.h"
#include "PlayerAI.h"
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Group;
class Player;
class SpellInfo;
class Unit;

namespace ForeverBots
{
    // relevance bands (compatible with the mod-playerbots scale)
    enum Relevance : int32
    {
        R_IDLE          = 0,
        R_MOVE          = 5,
        R_BUFF          = 8,
        R_ROTATION      = 10,   // 10..30 by the class profile's priority
        R_CC            = 35,
        R_HEAL          = 40,
        R_INTERRUPT     = 45,
        R_DISPEL        = 50,
        R_TAUNT         = 60,
        R_CRITICAL_HEAL = 70,
        R_EMERGENCY     = 90
    };

    enum class BotState : uint8
    {
        Idle,       // following / waiting
        Combat,
        Rest,       // no fight, recovering health and mana
        Dead
    };
    char const* BotStateName(BotState state);

    enum SpellKind : uint32
    {
        KIND_DIRECT_HEAL = 0x00001,
        KIND_HOT         = 0x00002,
        KIND_SHIELD      = 0x00004,
        KIND_AOE_HEAL    = 0x00008,
        KIND_DAMAGE      = 0x00010,
        KIND_DOT         = 0x00020,
        KIND_AOE_DAMAGE  = 0x00040,
        KIND_INTERRUPT   = 0x00080,
        KIND_DISPEL      = 0x00100,     // removes debuffs from friends
        KIND_PURGE       = 0x00200,     // removes buffs from enemies
        KIND_TAUNT       = 0x00400,
        KIND_CC          = 0x00800,     // stun, fear, polymorph, banish...
        KIND_RESURRECT   = 0x01000,
        KIND_BUFF        = 0x02000,     // long buff for allies
        KIND_SELF_BUFF   = 0x04000,     // long buff on the caster only
        KIND_DEFENSIVE   = 0x08000,     // cooldown that reduces damage taken / saves the caster
        KIND_THREAT_DROP = 0x10000,
        KIND_SNARE       = 0x20000,
        KIND_MELEE       = 0x40000,     // weapon strike
        KIND_GROUND      = 0x80000      // aimed at a position (Blizzard, Flamestrike...)
    };

    struct BotSpell
    {
        SpellInfo const* Info = nullptr;
        std::string Name;
        uint32 Kinds = 0;
        uint32 DispelMask = 0;      // KIND_DISPEL / KIND_PURGE
        int32 Amount = 0;           // estimated heal / damage of one cast (periodic effects: their whole duration)
        uint32 CastTime = 0;        // ms
        uint32 Cooldown = 0;        // ms
        float Range = 0.0f;
        bool Has(uint32 kind) const { return (Kinds & kind) != 0; }
    };

    // the spells a bot knows (best rank of each name), classified from their SpellInfo
    class BotSpellBook
    {
    public:
        void Build(Player* me);
        bool NeedsRebuild(Player const* me) const;
        BotSpell const* Find(std::string_view name) const;
        std::vector<BotSpell const*> Of(uint32 kinds) const;      // any of the kinds
        std::vector<BotSpell const*> const& All() const { return _ordered; }
        std::string Describe() const;

    private:
        std::unordered_map<std::string, BotSpell> _byName;
        std::vector<BotSpell const*> _ordered;
        uint8 _level = 0;
        size_t _spellCount = 0;
    };

    // what the members of one group share
    struct PartyBlackboard
    {
        struct Claim { ObjectGuid Bot; uint32 Expires = 0; };
        struct Heal { ObjectGuid Healer; int32 Amount = 0; uint32 Expires = 0; };

        ObjectGuid Tank;
        ObjectGuid Leader;                  // a real player, or the bot tank of a bot-only party
        ObjectGuid FocusTarget;             // the group's kill target (player order, skull mark, pull)
        uint32 FocusExpires = 0;
        ObjectGuid PullTarget;
        uint32 PullStarted = 0;
        std::unordered_map<ObjectGuid, Claim> Interrupts;        // enemy -> bot interrupting it
        std::unordered_map<ObjectGuid, Claim> CrowdControl;      // enemy -> bot controlling it
        std::unordered_map<ObjectGuid, std::vector<Heal>> Heals; // ally -> heals on the way
        std::unordered_map<ObjectGuid, uint32> EngagedAt;        // enemy -> when the group first saw it fight
        uint32 CombatSince = 0;
        uint32 LastUse = 0;

        int32 IncomingHeal(ObjectGuid target, uint32 now) const;
        void AddHeal(ObjectGuid target, ObjectGuid healer, int32 amount, uint32 expires);
        bool TryClaim(std::unordered_map<ObjectGuid, Claim>& claims, ObjectGuid enemy, ObjectGuid bot, uint32 now, uint32 duration);
        bool IsClaimedByOther(std::unordered_map<ObjectGuid, Claim> const& claims, ObjectGuid enemy, ObjectGuid bot, uint32 now) const;
        void Prune(uint32 now);
    };

    // one blackboard per group (or per owner without a group); used from map threads
    class PartyRegistry
    {
    public:
        static PartyRegistry& Instance();
        PartyBlackboard& For(ObjectGuid key, uint32 now);
        void Cleanup(uint32 now);
    private:
        std::mutex _lock;
        std::unordered_map<ObjectGuid, PartyBlackboard> _parties;
        uint32 _lastCleanup = 0;
    };

    struct Candidate
    {
        int32 Relevance = 0;
        char const* Name = "";
        std::function<bool()> Run;
        std::string Reason;
        Unit* Target = nullptr;
    };

    class BotBrain
    {
    public:
        void Clear() { _candidates.clear(); }
        void Propose(int32 relevance, char const* name, Unit* target, std::function<bool()> run, std::string reason = {});
        Candidate const* Execute();     // the candidate that ran, or null
        size_t Size() const { return _candidates.size(); }
    private:
        std::vector<Candidate> _candidates;
        Candidate _done;
    };

    // the world as one bot sees it this tick
    struct BotContext
    {
        Player* Me = nullptr;
        ForeverBot* Data = nullptr;
        PartyBlackboard* Party = nullptr;
        Player* Leader = nullptr;           // who the party follows (owner, or the bot tank of a bot-only party)
        Player* Tank = nullptr;
        std::vector<Player*> Members;       // alive, same map, me included
        std::vector<Player*> Dead;          // dead members with a corpse (not released)
        std::vector<Unit*> Enemies;         // units fighting the group
        uint32 Now = 0;
        bool GroupInCombat = false;
        bool AutonomousLeader = false;      // I lead a bot-only party (no player)

        BotRole RoleOf(Player const* member) const;
        bool IsHealer(Player const* member) const { return RoleOf(member) == BotRole::Heal; }
    };
}

class ForeverBotAI : public PlayerAI
{
public:
    explicit ForeverBotAI(Player* player);

    void UpdateAI(uint32 diff) override;
    void DamageDealt(Unit* victim, uint32& damage, DamageEffectType damageType) override;
    void HealDone(Unit* to, uint32& addhealth) override;

private:
    using BotContext = ForeverBots::BotContext;
    using BotSpell = ForeverBots::BotSpell;

    // controller (ForeverBotAI.cpp)
    ForeverBot* GetBotData() const;
    bool BuildContext(BotContext& ctx);
    void Decide(BotContext& ctx, uint32 elapsed);
    void LogDecision(BotContext const& ctx, ForeverBots::Candidate const* done);
    void UpdateDead(BotContext& ctx);

    // actions (BotActions.cpp)
    bool CanCast(BotSpell const* spell, Unit* target, bool checkRange = true) const;
    bool Cast(BotSpell const* spell, Unit* target);
    bool CastAt(BotSpell const* spell, Position const& pos);
    bool CastNamed(std::string_view name, Unit* target);
    bool HasAuraNamed(Unit const* target, std::string const& name, bool mine) const;
    bool HasAuraFrom(Unit const* target, BotSpell const* spell, bool mine) const;
    void EnsureAttacking(Unit* target, bool melee);
    void EnsureAmmo();                                      // arrows or bullets for a bow, crossbow or gun
    void EnsureHunterPet();                                 // call, revive or create the hunter's pet
    bool IsSpellBlocked(uint32 spellId, uint32 now) const;

    // combat (BotCombat.cpp)
    Unit* SelectTarget(BotContext& ctx);
    Unit* SelectTankTarget(BotContext& ctx);
    Unit* FindHuntTarget() const;
    BotRole EffectiveRole(BotContext const& ctx) const;
    float ThreatPctOnTarget(BotContext const& ctx, Unit* target) const;   // my threat vs the tank's, in %
    bool IsValidEnemy(Unit* unit) const;
    void ProposeTank(BotContext& ctx, Unit* target);
    void ProposeHealer(BotContext& ctx, bool emergencyOnly = false);
    void ProposeDps(BotContext& ctx, Unit* target, int32 relevanceOffset = 0);
    void ProposeInterrupts(BotContext& ctx);
    void ProposeDispels(BotContext& ctx);
    void ProposeDefensives(BotContext& ctx);
    void ProposeBuffs(BotContext& ctx);
    void ProposeResurrect(BotContext& ctx);
    void ProposeCrowdControl(BotContext& ctx, Unit* target);
    uint32 CountEnemiesNear(BotContext const& ctx, Unit const* center, float radius) const;

    // class knowledge (BotClassAI.cpp)
    bool IsRangedClass() const;
    bool IsCasterClass() const;
    float PreferredRange() const;
    void ProposeClassUpkeep(BotContext& ctx, Unit* target, bool inCombat);   // stances, forms, aspects, seals, armors, pets
    bool ProposeNamed(int32 relevance, char const* name, Unit* target, char const* reason);
    void ProposeClassRotation(BotContext& ctx, Unit* target, int32 offset);  // damage / threat priorities
    void ProposeGenericRotation(BotContext& ctx, Unit* target, int32 offset);

    // movement (BotMovement.cpp)
    void MoveInCombat(BotContext& ctx, Unit* target);
    void MoveOutOfCombat(BotContext& ctx);
    void ProposePull(BotContext& ctx);
    bool IsPartyReadyToPull(BotContext const& ctx, std::string* reason = nullptr) const;
    bool IsChasing(Unit* target) const;
    void ChaseTarget(Unit* target, float minRange, float maxRange, Optional<float> angle);
    void CheckStuck(Unit* target, float wantedRange, uint32 elapsed);
    void SnapToGround();                                    // a bot standing above or under the floor (straight moves in caves)
    void Wander(ForeverBot const& data, uint32 elapsed);
    void Rest(BotContext& ctx, uint32 elapsed);
    void ProposeConsumables(BotContext& ctx);               // healing and mana potions in a fight

    ForeverBots::BotSpellBook _spells;
    ForeverBots::BotBrain _brain;
    ForeverBots::BotState _state = ForeverBots::BotState::Idle;
    uint32 _thinkTimer = 0;
    uint32 _thinkInterval = 300;
    uint32 _outOfCombatTime = 0;
    uint32 _wanderTimer = 0;
    uint32 _slowTimer = 0;
    std::unordered_map<uint32, uint32> _blockedSpells;      // spell -> blocked until (failed cast grace)
    std::unordered_map<uint64, uint32> _failLogged;         // debug: spell+reason -> next log
    float _followLastDist = 0.0f;                           // follow progress (out of combat)
    uint32 _followStuckTime = 0;
    ObjectGuid _losBlocked;                                 // the server refused a cast on it for line of sight
    uint32 _losBlockedUntil = 0;
    std::unordered_map<ObjectGuid, uint32> _unreachable;    // enemy -> ignored until
    float _chaseMin = -1.0f;                                // the range of the chase in progress (ChaseTarget)
    float _chaseMax = -1.0f;
    Optional<float> _chaseAngle;
    uint32 _healBlockedLog = 0;                             // debug: next "can't heal" log
    uint32 _petRetry = 0;                                   // next try to get the hunter's pet
    uint32 _autoShotRetry = 0;                              // next Auto Shot start after a refusal
    uint32 _snapUntil = 0;                                  // stepping back onto the floor: no other movement until then
    uint32 _potionReadyAt = 0;                              // potions share a two minute cooldown

    // stuck detection
    ObjectGuid _stuckTarget;
    float _stuckLastDist = 0.0f;
    uint32 _stuckTime = 0;

    // debug
    std::string _lastDecision;
    uint32 _lastDecisionLog = 0;
};

#endif
