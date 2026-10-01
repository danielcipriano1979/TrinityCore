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
    bool SummonRequested = false;

    // world bots
    bool World = false;
    uint8 WorldLevel = 0;           // level to give at the next login (0 = keep)
    Position Home;                  // where it hunts
    uint32 RelogCheckTimer = 0;
    uint8 LoginLevel = 0;           // level when it last logged in

    bool IsWorldBot() const { return World; }
};

class ForeverBotMgr
{
public:
    static ForeverBotMgr* instance();

    // recruits a bot of that class next to the owner and puts it in the owner's group; returns an error or empty
    std::string AddBot(Player* owner, uint8 classId, BotRole role);
    void RemoveBot(ObjectGuid guid);
    void RemoveBotsOf(ObjectGuid owner);
    void RemoveAll();

    // world bots: keeps that many of them logged in (0 logs them all out)
    void SetWorldBotCount(uint32 count);
    uint32 GetWorldBotTarget() const { return _worldTarget; }
    uint32 CountWorldBots() const;

    std::vector<std::string> DescribeBots();             // one line per bot, for .botinfo
    std::vector<ForeverBot*> GetBotsOf(ObjectGuid owner);
    ForeverBot* GetBot(ObjectGuid guid);
    bool IsBot(ObjectGuid guid) const { return _bots.contains(guid); }

    void Update(uint32 diff);

private:
    uint32 GetBotAccountId();
    ObjectGuid CreateCharacter(uint8 race, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    ObjectGuid FindOrCreateCharacter(Player* owner, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    void PlaceCharacterAt(ObjectGuid guid, Player* owner);
    void PlaceCharacterAt(ObjectGuid guid, uint32 mapId, Position const& pos);
    void EnsureCustomizations(ObjectGuid guid);
    void StartLogin(ForeverBot& bot, Player* owner);
    void StartWorldLogin(ForeverBot& bot);
    void LoginWhenSaved(ForeverBot& bot, CharacterDatabaseTransaction& pendingCommit, bool world);
    void PrepareBeforeWorld(ObjectGuid guid, Player* botPlayer);
    void InitializeInWorld(ForeverBot& bot, Player* botPlayer, Player* owner);
    void LearnClassSpells(Player* bot);
    void AutoGear(Player* bot, BotRole role);
    void SpendTalents(Player* bot, BotRole role);
    void Teleport(ForeverBot& bot, Player* botPlayer, Player* owner);
    void Relog(ForeverBot& bot, Player* owner);
    void Logout(ForeverBot& bot);

    void UpdatePartyBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& toRemove);
    void UpdateWorldBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& toRemove);
    void BalanceWorldBots();
    bool PickWorldSpot(uint8 level, uint32& mapId, Position& pos);
    void BuildWorldSpots();

    struct WorldSpot { uint16 MapId; uint8 MinLevel; uint8 MaxLevel; Position Pos; };

    std::unordered_map<ObjectGuid, ForeverBot> _bots;
    uint32 _accountId = 0;
    uint32 _worldTarget = 0;
    uint32 _balanceTimer = 0;
    std::vector<WorldSpot> _worldSpots;
};

#define sForeverBotMgr ForeverBotMgr::instance()

class ForeverBotAI : public PlayerAI
{
public:
    explicit ForeverBotAI(Player* player) : PlayerAI(player) { }

    void UpdateAI(uint32 diff) override;

private:
    ForeverBot* GetBotData() const;
    Player* GetOwner() const;
    std::vector<Player*> GetGroupMembers(Player* owner) const;
    Unit* SelectTarget(ForeverBot const& data, Player* owner, std::vector<Player*> const& members) const;
    Unit* FindHuntTarget() const;
    bool DoHeal(std::vector<Player*> const& members);
    bool DoBuffs(std::vector<Player*> const& members);
    bool DoAttackSpells(Unit* target, ForeverBot const& data);
    bool TryCast(Unit* target, char const* spellName, bool requireNoAura = false);
    SpellInfo const* FindBestSpell(char const* spellName) const;
    void MoveForCombat(Unit* target, ForeverBot const& data);
    void FollowOwner(Player* owner, ForeverBot const& data);
    void Wander(ForeverBot const& data);
    void RegenOutOfCombat(uint32 elapsed);
    bool IsCaster() const;
    bool IsRanged() const;

    uint32 _thinkTimer = 0;
    uint32 _outOfCombatTime = 0;
    uint32 _wanderTimer = 0;
    std::unordered_map<std::string, uint32> _spellCache;   // spell name -> best known rank (0 = unknown)
    uint8 _cachedLevel = 0;
};

#endif
