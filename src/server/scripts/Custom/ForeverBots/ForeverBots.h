/*
 * Forever bots: real player characters driven by the server, recruited by a player into their group.
 *
 * Each bot is a character of the FOREVERBOTS account, logged in through a WorldSession without any socket
 * (WorldSession::SetBotSession). The manager owns those sessions (they are not in World's session map), keeps them
 * next to their owner (a bot that is on another map logs out and back in at the owner's position) and gives each
 * bot a ForeverBotAI that follows, assists and casts.
 */

#ifndef FOREVER_BOTS_H
#define FOREVER_BOTS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "PlayerAI.h"
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
    ObjectGuid Owner;
    WorldSession* Session = nullptr;
    BotRole Role = BotRole::Dps;
    BotOrder Order = BotOrder::Follow;
    bool Passive = false;
    uint8 Index = 0;                // position around the owner
    bool Ready = false;             // in world, leveled, grouped, AI set
    bool Relogging = false;         // moving to the owner's map through a logout/login
    bool Creating = false;          // new character still being saved
    bool CreateFailed = false;
    uint32 DeadTimer = 0;
    bool SummonRequested = false;
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

    std::vector<ForeverBot*> GetBotsOf(ObjectGuid owner);
    ForeverBot* GetBot(ObjectGuid guid);
    bool IsBot(ObjectGuid guid) const { return _bots.contains(guid); }

    void Update(uint32 diff);

private:
    uint32 GetBotAccountId();
    ObjectGuid FindOrCreateCharacter(Player* owner, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
        CharacterDatabaseTransaction& pendingCommit);
    void PlaceCharacterAt(ObjectGuid guid, Player* owner);
    void StartLogin(ForeverBot& bot, Player* owner);
    void PrepareBeforeWorld(ObjectGuid guid, Player* botPlayer);
    void InitializeInWorld(ForeverBot& bot, Player* botPlayer, Player* owner);
    void LearnClassSpells(Player* bot);
    void Teleport(ForeverBot& bot, Player* botPlayer, Player* owner);
    void Logout(ForeverBot& bot);

    std::unordered_map<ObjectGuid, ForeverBot> _bots;
    uint32 _accountId = 0;
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
    bool DoHeal(std::vector<Player*> const& members);
    bool DoBuffs(std::vector<Player*> const& members);
    bool DoAttackSpells(Unit* target, ForeverBot const& data);
    bool TryCast(Unit* target, char const* spellName, bool requireNoAura = false);
    SpellInfo const* FindBestSpell(char const* spellName) const;
    void MoveForCombat(Unit* target, ForeverBot const& data);
    void FollowOwner(Player* owner, ForeverBot const& data);
    bool IsCaster() const;
    bool IsRanged() const;

    uint32 _thinkTimer = 0;
    uint32 _outOfCombatTime = 0;
    std::unordered_map<std::string, uint32> _spellCache;   // spell name -> best known rank (0 = unknown)
    uint8 _cachedLevel = 0;
};

#endif
