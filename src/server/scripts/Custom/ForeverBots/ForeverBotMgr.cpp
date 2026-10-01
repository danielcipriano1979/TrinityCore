/*
 * Forever bots: the manager (bot sessions, characters, group, level, death and map changes).
 * Everything here runs on the world thread (WorldScript::OnUpdate and chat commands), outside map updates.
 */

#include "ForeverBots.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "RealmList.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
    constexpr char const* BotAccountName = "FOREVERBOTS";
    constexpr uint32 MaxBotsPerOwner = 4;
    constexpr uint32 ResurrectDelay = 5000;
    constexpr float RegroupDistance = 120.0f;

    uint8 const HordeRaces[] = { 2 /*orc*/, 5 /*undead*/, 6 /*tauren*/, 8 /*troll*/ };
    uint8 const AllianceRaces[] = { 1 /*human*/, 3 /*dwarf*/, 4 /*night elf*/, 7 /*gnome*/ };

    char const* const NameStarts[] = { "Ka", "Mor", "Thra", "Gru", "Zul", "Vor", "Bel", "Dra", "Ser", "Ul", "Gar", "Nok", "Rha", "Tor", "Jin",
        "Bro", "Ela", "Fen", "Kor", "Lia", "Mag", "Ora", "Sha", "Tal", "Vel", "Yri", "Azu", "Dun", "Hel", "Iso" };
    char const* const NameEnds[] = { "gar", "rok", "zan", "thas", "dor", "mir", "nok", "ria", "las", "gon", "rek", "ven", "dra", "mok",
        "lin", "tha", "ros", "zul", "nar", "wyn", "bek", "dil", "sha", "tor", "via", "gul", "ren", "kan" };
}

char const* BotRoleName(BotRole role)
{
    switch (role)
    {
        case BotRole::Tank: return "tank";
        case BotRole::Heal: return "heal";
        default:            return "dps";
    }
}

ForeverBotMgr* ForeverBotMgr::instance()
{
    static ForeverBotMgr instance;
    return &instance;
}

uint32 ForeverBotMgr::GetBotAccountId()
{
    if (_accountId)
        return _accountId;

    _accountId = AccountMgr::GetId(BotAccountName);
    if (!_accountId)
    {
        std::string password;
        for (int i = 0; i < 16; ++i)
            password += char('a' + urand(0, 25));
        sAccountMgr->CreateAccount(BotAccountName, password);
        _accountId = AccountMgr::GetId(BotAccountName);
    }
    return _accountId;
}

static WorldSession* CreateBotSession(uint32 accountId)
{
    uint32 build = 0;
    if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
        build = realm->Build;

    WorldSession* session = new WorldSession(accountId, BotAccountName, 0, "", nullptr, SEC_PLAYER,
        uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "", Minutes(0), build, ClientBuild::VariantId{}, LOCALE_enUS, 0, false);
    session->SetBotSession(true);
    return session;
}

static std::string GenerateBotName()
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        std::string name = std::string(Trinity::Containers::SelectRandomContainerElement(NameStarts))
            + Trinity::Containers::SelectRandomContainerElement(NameEnds);
        if (ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
            continue;
        if (sCharacterCache->GetCharacterCacheByName(name) || sObjectMgr->IsReservedName(name))
            continue;
        return name;
    }
    return {};
}

ObjectGuid ForeverBotMgr::FindOrCreateCharacter(Player* owner, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
    CharacterDatabaseTransaction& pendingCommit)
{
    bool const horde = owner->GetTeam() == HORDE;
    std::vector<uint8> races;
    for (uint8 race : horde ? std::span<uint8 const>(HordeRaces) : std::span<uint8 const>(AllianceRaces))
        if (sObjectMgr->GetPlayerInfo(race, classId))
            races.push_back(race);

    if (races.empty())
    {
        error = "That class is not available for your faction.";
        return ObjectGuid::Empty;
    }

    // reuse a free character of that class and faction
    std::string raceList;
    for (uint8 race : races)
        raceList += (raceList.empty() ? "" : ",") + std::to_string(race);

    if (QueryResult result = CharacterDatabase.Query(Trinity::StringFormat("SELECT guid FROM characters WHERE account = {} AND class = {} AND race IN ({}) AND deleteDate IS NULL",
        accountId, classId, raceList).c_str()))
    {
        do
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>((*result)[0].GetUInt64());
            if (!_bots.contains(guid))
                return guid;
        } while (result->NextRow());
    }

    // create a new one
    std::string name = GenerateBotName();
    if (name.empty())
    {
        error = "Could not find a free bot name.";
        return ObjectGuid::Empty;
    }

    WorldPackets::Character::CharacterCreateInfo createInfo;
    createInfo.Race = Trinity::Containers::SelectRandomContainerElement(races);
    createInfo.Class = classId;
    createInfo.Sex = uint8(urand(0, 1));
    createInfo.Name = name;

    Player* newChar = new Player(session);
    newChar->GetMotionMaster()->Initialize();
    if (!newChar->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
    {
        newChar->CleanupsBeforeDelete();
        delete newChar;
        error = "The bot character could not be created.";
        return ObjectGuid::Empty;
    }

    // the character statements are prepared on the async connections only (a synchronous commit asserts on m_mStmt):
    // the caller commits pendingCommit asynchronously and logs the bot in once it is done
    LoginDatabaseTransaction loginTransaction = LoginDatabase.BeginTransaction();
    pendingCommit = CharacterDatabase.BeginTransaction();
    newChar->SaveToDB(loginTransaction, pendingCommit, true);
    LoginDatabase.CommitTransaction(loginTransaction);

    ObjectGuid guid = newChar->GetGUID();
    sCharacterCache->AddCharacterCacheEntry(guid, accountId, name, createInfo.Sex, createInfo.Race, classId, newChar->GetLevel(), false);

    newChar->CleanupsBeforeDelete();
    delete newChar;

    TC_LOG_INFO("misc", "ForeverBots: created bot character {} ({}) race {} class {}", name, guid.ToString(), createInfo.Race, classId);
    return guid;
}

void ForeverBotMgr::PlaceCharacterAt(ObjectGuid guid, Player* owner)
{
    // the bot logs in where the owner is (no teleport needed, works across maps and instances)
    Position pos = owner->GetNearPosition(2.0f, float(M_PI));
    CharacterDatabase.DirectExecute(Trinity::StringFormat(
        "UPDATE characters SET map = {}, instance_id = {}, zone = {}, position_x = {}, position_y = {}, position_z = {}, orientation = {} WHERE guid = {}",
        owner->GetMapId(), owner->GetInstanceId(), owner->GetZoneId(), pos.GetPositionX(), pos.GetPositionY(), owner->GetPositionZ(), owner->GetOrientation(),
        guid.GetCounter()).c_str());
}

std::string ForeverBotMgr::AddBot(Player* owner, uint8 classId, BotRole role)
{
    std::vector<ForeverBot*> current = GetBotsOf(owner->GetGUID());
    if (current.size() >= MaxBotsPerOwner)
        return Trinity::StringFormat("You already have {} bots.", MaxBotsPerOwner);

    if (Group* group = owner->GetGroup())
        if (group->IsFull() && group->isRaidGroup())
            return "Your group is full.";

    uint32 accountId = GetBotAccountId();
    if (!accountId)
        return "Could not create the bot account.";

    WorldSession* session = CreateBotSession(accountId);
    std::string error;
    CharacterDatabaseTransaction pendingCommit;
    ObjectGuid guid = FindOrCreateCharacter(owner, classId, accountId, session, error, pendingCommit);
    if (guid.IsEmpty())
    {
        delete session;
        return error;
    }

    // free position index around the owner
    uint8 index = 0;
    for (bool used = true; used; )
    {
        used = false;
        for (ForeverBot const* other : current)
            if (other->Index == index)
            {
                used = true;
                ++index;
                break;
            }
    }

    ForeverBot& bot = _bots[guid];
    bot.Guid = guid;
    bot.Owner = owner->GetGUID();
    bot.Session = session;
    bot.Role = role;
    bot.Index = index;

    if (!pendingCommit)
    {
        PlaceCharacterAt(guid, owner);
        session->LoginBotCharacter(guid);
        return {};
    }

    // new character: log in once it is saved (the callback runs from the session update, see Update)
    bot.Creating = true;
    session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(pendingCommit)).AfterComplete([this, guid](bool success)
    {
        ForeverBot* created = GetBot(guid);
        if (!created)
            return;

        created->Creating = false;
        Player* owner = ObjectAccessor::FindConnectedPlayer(created->Owner);
        if (!success || !owner)
        {
            created->CreateFailed = true;
            return;
        }

        PlaceCharacterAt(guid, owner);
        created->Session->LoginBotCharacter(guid);
    });
    return {};
}

void ForeverBotMgr::LearnClassSpells(Player* bot)
{
    ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(bot->GetClass());
    if (!classEntry)
        return;

    // what `.learn my spells` does, limited to the bot's level (trainers would sell these)
    for (SkillLineAbilityEntry const* ability : sSkillLineAbilityStore)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(ability->Spell, DIFFICULTY_NONE);
        if (!spellInfo || spellInfo->SpellLevel == 0 || spellInfo->SpellLevel > bot->GetLevel())
            continue;
        if (spellInfo->SpellFamilyName != classEntry->SpellClassSet || !bot->IsSpellFitByClassAndRace(spellInfo->Id))
            continue;
        if (bot->HasSpell(spellInfo->Id) || !SpellMgr::IsSpellValid(spellInfo, bot, false))
            continue;
        bot->LearnSpell(spellInfo->Id, false, 0, true);
    }
}

void ForeverBotMgr::InitializeInWorld(ForeverBot& bot, Player* botPlayer, Player* owner)
{
    if (botPlayer->GetLevel() != owner->GetLevel())
        botPlayer->GiveLevel(owner->GetLevel());
    LearnClassSpells(botPlayer);

    botPlayer->ResurrectPlayer(1.0f);
    botPlayer->SetFullHealth();
    botPlayer->SetFullPower(botPlayer->GetPowerType());

    Group* group = owner->GetGroup();
    if (!group)
    {
        group = new Group();
        if (!group->Create(owner))
        {
            delete group;
            return;
        }
        sGroupMgr->AddGroup(group);
    }

    if (botPlayer->GetGroup() && botPlayer->GetGroup() != group)
        botPlayer->RemoveFromGroup();

    if (!group->IsMember(botPlayer->GetGUID()))
    {
        if (group->IsFull() && !group->isRaidGroup())
            group->ConvertToRaid();
        group->AddMember(botPlayer);
    }
    group->BroadcastGroupUpdate();

    if (!botPlayer->AI())
        botPlayer->SetBotAI(new ForeverBotAI(botPlayer));

    bot.Ready = true;
    bot.Relogging = false;
    ChatHandler(owner->GetSession()).PSendSysMessage("ForeverBots: {} joins as {}.", botPlayer->GetName(), BotRoleName(bot.Role));
}

void ForeverBotMgr::Teleport(ForeverBot& bot, Player* botPlayer, Player* owner)
{
    if (botPlayer->GetMapId() != owner->GetMapId())
        return;

    Position pos = owner->GetNearPosition(2.0f + bot.Index * 0.5f, float(M_PI) - 0.6f + bot.Index * 0.4f);
    pos.m_positionZ = owner->GetPositionZ();
    botPlayer->GetMotionMaster()->Clear();
    botPlayer->NearTeleportTo(pos);
    bot.Session->FinishBotTeleport();
}

void ForeverBotMgr::Logout(ForeverBot& bot)
{
    if (!bot.Session)
        return;

    bot.Session->LogoutPlayer(true);
    delete bot.Session;
    bot.Session = nullptr;
    bot.Ready = false;
}

void ForeverBotMgr::RemoveBot(ObjectGuid guid)
{
    auto itr = _bots.find(guid);
    if (itr == _bots.end())
        return;

    if (itr->second.Session)
        if (Player* botPlayer = itr->second.Session->GetPlayer())
            if (botPlayer->GetGroup())
                botPlayer->RemoveFromGroup();

    Logout(itr->second);
    _bots.erase(itr);
}

void ForeverBotMgr::RemoveBotsOf(ObjectGuid owner)
{
    for (ForeverBot* bot : GetBotsOf(owner))
        RemoveBot(bot->Guid);
}

void ForeverBotMgr::RemoveAll()
{
    std::vector<ObjectGuid> guids;
    for (auto const& [guid, bot] : _bots)
        guids.push_back(guid);
    for (ObjectGuid guid : guids)
        RemoveBot(guid);
}

std::vector<ForeverBot*> ForeverBotMgr::GetBotsOf(ObjectGuid owner)
{
    std::vector<ForeverBot*> result;
    for (auto& [guid, bot] : _bots)
        if (bot.Owner == owner)
            result.push_back(&bot);
    return result;
}

ForeverBot* ForeverBotMgr::GetBot(ObjectGuid guid)
{
    auto itr = _bots.find(guid);
    return itr != _bots.end() ? &itr->second : nullptr;
}

void ForeverBotMgr::Update(uint32 diff)
{
    std::vector<ObjectGuid> toRemove;

    for (auto& [guid, bot] : _bots)
    {
        Player* owner = ObjectAccessor::FindConnectedPlayer(bot.Owner);
        if (!owner)
        {
            toRemove.push_back(guid);
            continue;
        }

        if (!bot.Session)
        {
            toRemove.push_back(guid);
            continue;
        }

        // the world pass of the session: runs the login query callback
        WorldSessionFilter filter(bot.Session);
        bot.Session->Update(diff, filter);

        Player* botPlayer = bot.Session->GetPlayer();
        if (!botPlayer || !botPlayer->IsInWorld())
        {
            if (bot.CreateFailed || (!bot.Creating && !bot.Session->PlayerLoading() && !botPlayer))
            {
                TC_LOG_ERROR("misc", "ForeverBots: bot {} could not log in", guid.ToString());
                ChatHandler(owner->GetSession()).SendSysMessage("ForeverBots: a bot could not log in.");
                toRemove.push_back(guid);
            }
            continue;
        }

        if (!owner->IsInWorld() || owner->IsBeingTeleported())
            continue;

        // the owner went to another map or instance: log the bot out and back in at the owner's position
        if (botPlayer->GetMapId() != owner->GetMapId() || botPlayer->GetInstanceId() != owner->GetInstanceId())
        {
            Logout(bot);
            PlaceCharacterAt(guid, owner);
            bot.Session = CreateBotSession(GetBotAccountId());
            bot.Relogging = true;
            bot.Session->LoginBotCharacter(guid);
            continue;
        }

        if (!bot.Ready)
        {
            InitializeInWorld(bot, botPlayer, owner);
            continue;
        }

        if (botPlayer->GetLevel() < owner->GetLevel())
        {
            botPlayer->GiveLevel(owner->GetLevel());
            LearnClassSpells(botPlayer);
        }

        if (botPlayer->isDead())
        {
            bot.DeadTimer += diff;
            if (bot.DeadTimer >= ResurrectDelay && owner->IsAlive() && !owner->IsInCombat())
            {
                botPlayer->ResurrectPlayer(0.5f);
                botPlayer->SpawnCorpseBones();
                Teleport(bot, botPlayer, owner);
                bot.DeadTimer = 0;
            }
            continue;
        }
        bot.DeadTimer = 0;

        if (bot.SummonRequested || (!botPlayer->IsInCombat() && !botPlayer->IsWithinDistInMap(owner, RegroupDistance)))
        {
            bot.SummonRequested = false;
            Teleport(bot, botPlayer, owner);
        }
    }

    for (ObjectGuid guid : toRemove)
        RemoveBot(guid);
}
