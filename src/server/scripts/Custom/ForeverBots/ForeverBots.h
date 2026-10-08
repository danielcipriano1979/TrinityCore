/*
 * Forever bots: real player characters driven by the server.
 *
 * Party bots are recruited by a player (.bot add / the ForeverBots addon) into their group. World bots
 * (.bot world <count>) live on their own: they log in around Kalimdor and the Eastern Kingdoms, hunt
 * creatures of their level and level up, so the world doesn't feel empty.
 *
 * Each bot is a character of the FOREVERBOTS account, logged in through a WorldSession without any socket
 * (WorldSession::SetBotSession). The manager owns those sessions (they are not in World's session map).
 *
 * The 1.60 client is fragile with some update packets about *other* players (a full party member values
 * update crashes it), so every heavy change - level, spells, gear, appearance, group membership - is done in
 * PrepareBeforeWorld, during the bot's login and before anyone can see it. Leveling up or changing map is done
 * by logging the bot out and back in.
 */

#ifndef FOREVER_BOTS_H
#define FOREVER_BOTS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerAI.h"
#include "Position.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Unit;
class WorldSession;
class SpellInfo;

enum class BotRole : uint8
{
    Tank,
    Heal,
    Dps
};

enum class BotOrder : uint8
{
    Follow,     // follow the owner, fight what the group fights
    Stay,       // hold position, still fight from there
    Attack      // attack the owner's selected target now
};

char const* BotRoleName(BotRole role);

struct ForeverBot
{
    ObjectGuid Guid;
    ObjectGuid Owner;               // empty for world bots
    WorldSession* Session = nullptr;
    BotRole Role = BotRole::Dps;
    BotOrder Order = BotOrder::Follow;
    bool Passive = false;
    uint8 Index = 0;                // position around the owner
    bool Ready = false;             // in world, prepared, AI set
    bool Relogging = false;         // logging out and back in (map change, level up)
    bool Creating = false;          // new character still being saved
    bool CreateFailed = false;
    uint32 DeadTimer = 0;
    uint32 CorpseRunTimer = 0;      // wipe: time since this bot released to run back to the dungeon
    uint32 GearCheckTimer = 0;      // next look at the bags for an upgrade won on a roll
    bool SummonRequested = false;

    // world bots
    bool World = false;
    uint8 WorldLevel = 0;           // level to give at the next login (0 = keep)
    Position Home;                  // where it hunts
    uint32 RelogCheckTimer = 0;
    uint8 LoginLevel = 0;           // level when it last logged in
    uint32 LastXp = 0;              // progress watchdog
    uint32 StuckTime = 0;

    // bot-only party (.bottest): this bot leads, the others are owned by it
    bool Leader = false;
    std::vector<std::pair<uint8, BotRole>> PendingRecruits;    // members to recruit once the leader is in the world

    // diagnostics (.botinfo)
    uint32 LastCastSpell = 0;
    int32 LastCastResult = -1;
    std::string LastTarget;
    bool LastAttackOk = false;
    uint32 Kills = 0;
    bool Debug = false;                 // log every decision (BOT_DEBUG)
    std::string Decision;               // current decision and reason
    std::string State;

    // telemetry
    uint64 DamageDone = 0;
    uint64 HealingDone = 0;
    uint64 Overhealing = 0;
    uint32 Deaths = 0;
    uint32 Interrupts = 0;
    uint32 Dispels = 0;
    uint32 Taunts = 0;
    uint32 CrowdControls = 0;
    uint32 Resurrections = 0;
    uint32 CastsOk = 0;
    uint32 CastsFailed = 0;
    uint32 CombatTime = 0;              // ms

    bool IsWorldBot() const { return World; }
};

class ForeverBotMgr
{
public:
    static ForeverBotMgr* instance();

    // recruits a bot of that class next to the owner and puts it in the owner's group; returns an error or empty
    // maxBots: how many bots the owner may have (0 = the default, a party of five; raids of the Group Finder ask for more)
    std::string AddBot(Player* owner, uint8 classId, BotRole role, uint32 maxBots = 0);
    void RemoveBot(ObjectGuid guid);
    void RemoveBotsOf(ObjectGuid owner);
    void RemoveAll();

    // world bots: keeps that many of them logged in (0 logs them all out)
    void SetWorldBotCount(uint32 count, bool persist = true);
    void LoadWorldBotCount(uint32 configDefault);     // the count saved by the last .bot world, else the config
    uint32 GetWorldBotTarget() const { return _worldTarget; }
    uint32 CountWorldBots() const;

    std::vector<std::string> DescribeBots();             // one line per bot, for .botinfo
    std::vector<std::string> DescribeTelemetry();        // one line per bot: role, state, decision, counters
    std::vector<std::string> DescribeStats();            // one line per bot: health, armor, stats, attack power, weapon, gear
    std::vector<ForeverBot*> GetBotsOf(ObjectGuid owner);
    ForeverBot* GetBot(ObjectGuid guid);
    bool IsBot(ObjectGuid guid) const { return _bots.contains(guid); }
    void SetDebug(bool on);                              // every bot logs its decisions
    bool IsGlobalDebug() const { return _debug; }

    // the Classic spells of each class: class trainers, talents and the level 1 spells. The client data also
    // carries later expansions' class spells, which a Classic character never learns. Built at startup, read-only.
    void LoadClassicSpells();
    bool IsClassicSpell(uint8 classId, SpellInfo const* spellInfo) const;

    // bot-only party for tests: a leader (first entry) and its members, logged in at that place
    std::string StartTestParty(uint32 mapId, Position const& pos, uint8 level, uint8 team,
        std::vector<std::pair<uint8, BotRole>> const& members);
    void StopTestParties();
    static uint32 TanksFor(uint32 size);
    // the roles a player ticked for their last Group Finder run (lfg::LfgRoles mask, 0 = none)
    uint8 GetOwnerRoles(ObjectGuid owner) const { auto itr = _ownerRoles.find(owner); return itr != _ownerRoles.end() ? itr->second : 0; }
    static uint32 HealersFor(uint32 size);

    // Group Finder (the game's own window): the player posted a group for a dungeon or a raid. Takes them to its entrance,
    // inside, and recruits the bots the group is missing, up to the size of the activity (5, 10, 20, 40: tanks, healers, the
    // rest damage dealers). roles = what the player ticked (lfg::LfgRoles). Returns false when none of the activities is in
    // forever_groupfinder_dungeon.
    bool StartDungeonRun(Player* owner, uint32 category, std::vector<uint32> const& activities, uint8 roles);

    void Update(uint32 diff);

private:
    uint32 GetBotAccountId();
    ObjectGuid CreateCharacter(uint8 race, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    ObjectGuid FindOrCreateCharacter(Player* owner, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    void PlaceCharacterAt(ObjectGuid guid, Player* owner);
    void PlaceCharacterAt(ObjectGuid guid, uint32 mapId, Position const& pos);
    void EnsureCustomizations(ObjectGuid guid, WorldSession* session);
    void StartLogin(ForeverBot& bot, Player* owner);
    void StartWorldLogin(ForeverBot& bot);
    void LoginWhenSaved(ForeverBot& bot, CharacterDatabaseTransaction& pendingCommit, bool world);
    void PrepareBeforeWorld(ObjectGuid guid, Player* botPlayer);
    void InitializeInWorld(ForeverBot& bot, Player* botPlayer, Player* owner);
    void LearnClassSpells(Player* bot);
    void AutoGear(Player* bot, BotRole role);
    void SpendTalents(Player* bot, BotRole role);
    void MaxCombatSkills(Player* bot);
    void Teleport(ForeverBot& bot, Player* botPlayer, Player* owner);
    void Relog(ForeverBot& bot, Player* owner);
    void Logout(ForeverBot& bot);

    void UpdatePartyBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& toRemove);
    void UpdateWorldBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& toRemove);
    void UpdateLeaderBot(ForeverBot& bot, uint32 diff);
    void UpdateDungeonRuns(uint32 diff);
    void UpdateLootRolls(ForeverBot& bot, Player* botPlayer);
    bool WantsItem(Player* botPlayer, BotRole role, uint32 itemId) const;     // an upgrade over what the bot wears
    bool HasUpgradeInBags(Player* botPlayer, BotRole role) const;
    ObjectGuid FindOrCreateCharacter(uint8 team, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    void BalanceWorldBots();
    bool PickWorldSpot(uint8 level, uint32& mapId, Position& pos);
    void BuildWorldSpots();

    struct WorldSpot { uint16 MapId; uint8 MinLevel; uint8 MaxLevel; Position Pos; };

    // a Group Finder run on its way: the owner is teleported a moment after posting the group, the bots are recruited once
    // they arrive
    struct DungeonRun
    {
        WorldLocation Entrance;
        uint8 Roles = 0;
        uint8 Size = 5;                                         // members the group is filled up to
        uint32 Timer = 0;
        bool Teleported = false;
        bool Planned = false;                                   // Recruits was built (the owner is inside)
        std::vector<std::pair<uint8, BotRole>> Recruits;        // still to log in, a few per second
        uint32 RecruitTimer = 0;
        uint32 Added = 0;
    };

    std::unordered_map<ObjectGuid, ForeverBot> _bots;
    std::unordered_map<ObjectGuid, DungeonRun> _dungeonRuns;
    std::unordered_map<ObjectGuid, uint8> _ownerRoles;
    uint32 _accountId = 0;
    uint32 _worldTarget = 0;
    uint32 _balanceTimer = 0;
    std::vector<WorldSpot> _worldSpots;
    bool _debug = false;
    std::unordered_map<uint8, std::unordered_map<uint32, uint8>> _trainerSpells;   // class -> spell -> trainer level
    std::unordered_set<uint32> _talentSpells;
    std::unordered_set<uint32> _starterSpells;
    std::unordered_map<uint32, uint8> _classBySpellFamily;
    void RemoveNonClassicSpells(Player* bot);
};

#define sForeverBotMgr ForeverBotMgr::instance()

#endif
