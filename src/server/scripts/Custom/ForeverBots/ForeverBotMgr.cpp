/*
 * Forever bots: the manager (bot sessions, characters, appearance, gear, group, level, death, map changes and
 * world bots). Everything here runs on the world thread (WorldScript::OnUpdate and chat commands), outside map
 * updates; PrepareBeforeWorld runs inside a bot's login (also the world thread, from the session update).
 */

#include "ForeverBots.h"
#include "AI/BotAI.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "Config.h"
#include "Chat.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Group.h"
#include "GroupFinderListings.h"
#include "GroupMgr.h"
#include "Item.h"
#include "LFG.h"
#include "ItemTemplate.h"
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
#include "TraitMgr.h"
#include "TraitPacketsCommon.h"
#include "UpdateFields.h"
#include "World.h"
#include <ranges>
#include <set>
#include "WorldSession.h"

namespace
{
    constexpr char const* PartyAccountName = "FOREVERBOTS";
    constexpr char const* WorldAccountName = "FOREVERWORLD";
    constexpr uint32 MaxBotsPerOwner = 4;
    constexpr uint32 MaxWorldBots = 200;
    constexpr uint32 ResurrectDelay = 15000;          // time for a healer to resurrect it first
    constexpr uint32 WorldResurrectDelay = 20000;
    constexpr float RegroupDistance = 120.0f;
    constexpr uint32 DungeonRunTeleportDelay = 1500;        // ms between posting a group in the Group Finder and the teleport
    constexpr uint32 DungeonRunRecruitInterval = 350;       // ms between two bot logins of a Group Finder run
    constexpr uint32 BalanceInterval = 5000;
    constexpr uint32 MaxLoginsPerBalance = 3;       // spreads world bot logins over time
    constexpr uint32 MaxVanillaItemId = 24500;      // the client DB2s also carry later expansions' items

    uint8 const HordeRaces[] = { 2 /*orc*/, 5 /*undead*/, 6 /*tauren*/, 8 /*troll*/ };
    uint8 const AllianceRaces[] = { 1 /*human*/, 3 /*dwarf*/, 4 /*night elf*/, 7 /*gnome*/ };
    uint8 const BotClasses[] = { CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID };

    char const* const NameStarts[] = { "Ka", "Mor", "Thra", "Gru", "Zul", "Vor", "Bel", "Dra", "Ser", "Ul", "Gar", "Nok", "Rha", "Tor", "Jin",
        "Bro", "Ela", "Fen", "Kor", "Lia", "Mag", "Ora", "Sha", "Tal", "Vel", "Yri", "Azu", "Dun", "Hel", "Iso", "Bra", "Cel", "Eri", "Gwe",
        "Hal", "Ith", "Lor", "Myr", "Nai", "Qua", "Rok", "Syl", "Tha", "Urs", "Val", "Wre", "Xan", "Zer" };
    char const* const NameMiddles[] = { "", "", "", "a", "e", "i", "o", "ra", "li", "na", "do" };
    char const* const NameEnds[] = { "gar", "rok", "zan", "thas", "dor", "mir", "nok", "ria", "las", "gon", "rek", "ven", "dra", "mok",
        "lin", "tha", "ros", "zul", "nar", "wyn", "bek", "dil", "sha", "tor", "via", "gul", "ren", "kan", "dus", "mar", "eth", "ius" };

    enum ItemMod : int32 { MOD_AGILITY = 3, MOD_STRENGTH = 4, MOD_INTELLECT = 5, MOD_SPIRIT = 6, MOD_STAMINA = 7 };
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

static uint32 GetOrCreateAccount(char const* name)
{
    uint32 id = AccountMgr::GetId(name);
    if (!id)
    {
        std::string password;
        for (int i = 0; i < 16; ++i)
            password += char('a' + urand(0, 25));
        sAccountMgr->CreateAccount(name, password);
        id = AccountMgr::GetId(name);
    }
    return id;
}

uint32 ForeverBotMgr::GetBotAccountId()
{
    if (!_accountId)
        _accountId = GetOrCreateAccount(PartyAccountName);
    return _accountId;
}

static uint32 GetWorldAccountId()
{
    static uint32 id = 0;
    if (!id)
        id = GetOrCreateAccount(WorldAccountName);
    return id;
}

static WorldSession* CreateBotSession(uint32 accountId, char const* accountName)
{
    uint32 build = 0;
    if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
        build = realm->Build;

    WorldSession* session = new WorldSession(accountId, accountName, 0, "", nullptr, SEC_PLAYER,
        uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "", Minutes(0), build, ClientBuild::VariantId{}, LOCALE_enUS, 0, false);
    session->SetBotSession(true);
    return session;
}

static std::string GenerateBotName()
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        std::string name = std::string(Trinity::Containers::SelectRandomContainerElement(NameStarts))
            + Trinity::Containers::SelectRandomContainerElement(NameMiddles)
            + Trinity::Containers::SelectRandomContainerElement(NameEnds);
        if (name.size() > 12)
            continue;
        if (ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
            continue;
        if (sCharacterCache->GetCharacterCacheByName(name) || sObjectMgr->IsReservedName(name))
            continue;
        return name;
    }
    return {};
}

// WorldSession::MeetsChrCustomizationReq without anything earned (achievement, appearance, quest): what any new
// character of that race and class may pick. A requirement without the HasRequirements flag (10, 12, 4475...) marks
// what the Classic character creator never offers (the "Eye Style" options, NPC skins and eyes): the server accepts
// those, but the client asserts "Invalid Option ID 0" / "Invalid Choice ID 0" (ComponentUtils.cpp) on such a
// character and draws it without its gear
static bool IsFreeCustomizationReq(int32 reqId, uint8 race, uint8 classId)
{
    ChrCustomizationReqEntry const* req = sChrCustomizationReqStore.LookupEntry(reqId);
    if (!req)
        return true;
    if (!req->GetFlags().HasFlag(ChrCustomizationReqFlag::HasRequirements))
        return false;
    if (req->ClassMask && !(req->ClassMask & (1 << (classId - 1))))
        return false;
    if (!req->RaceMask.IsEmpty() && req->RaceMask != RACEMASK_ALL_v<int32, 2> && !req->RaceMask.HasRace(race))
        return false;
    return !req->AchievementID && !req->ItemModifiedAppearanceID && !req->QuestID;
}

// A random but valid look: one choice for every option of that race and sex. Without any, the client asserts
// "Invalid Option ID 0" and can't build the character's model.
static void RandomCustomizations(uint8 race, uint8 classId, uint8 sex, std::vector<std::pair<uint32, uint32>>& out)
{
    std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, sex);
    if (!options)
        return;

    for (ChrCustomizationOptionEntry const* option : *options)
    {
        if (!IsFreeCustomizationReq(option->ChrCustomizationReqID, race, classId))
            continue;
        std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
        if (!choices || choices->empty())
            continue;

        std::vector<ChrCustomizationChoiceEntry const*> free;
        for (ChrCustomizationChoiceEntry const* choice : *choices)
            if (IsFreeCustomizationReq(choice->ChrCustomizationReqID, race, classId))
                free.push_back(choice);
        if (free.empty())
            continue;

        out.emplace_back(option->ID, Trinity::Containers::SelectRandomContainerElement(free)->ID);
    }
}

// A random look the server itself accepts: Player::LoadFromDB rejects a character whose choices fail
// WorldSession::ValidateAppearance (dependent choices, sorting...), so every candidate goes through it. No valid set
// -> no customizations at all (the client then shows a default look, but the bot logs in).
static void PickValidCustomizations(WorldSession* session, uint8 race, uint8 classId, uint8 sex, std::vector<std::pair<uint32, uint32>>& out)
{
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        out.clear();
        RandomCustomizations(race, classId, sex, out);
        std::ranges::sort(out);

        std::vector<UF::ChrCustomizationChoice> choices;
        for (auto const& [optionId, choiceId] : out)
        {
            UF::ChrCustomizationChoice& choice = choices.emplace_back();
            choice.ChrCustomizationOptionID = optionId;
            choice.ChrCustomizationChoiceID = choiceId;
        }
        if (session->ValidateAppearance(Races(race), Classes(classId), Gender(sex), { choices.data(), choices.data() + choices.size() }))
            return;
    }
    out.clear();
}

ObjectGuid ForeverBotMgr::CreateCharacter(uint8 race, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
    CharacterDatabaseTransaction& pendingCommit)
{
    std::string name = GenerateBotName();
    if (name.empty())
    {
        error = "Could not find a free bot name.";
        return ObjectGuid::Empty;
    }

    WorldPackets::Character::CharacterCreateInfo createInfo;
    createInfo.Race = race;
    createInfo.Class = classId;
    createInfo.Sex = uint8(urand(0, 1));
    createInfo.Name = name;

    std::vector<std::pair<uint32, uint32>> customizations;
    PickValidCustomizations(session, race, classId, createInfo.Sex, customizations);
    for (auto const& [optionId, choiceId] : customizations)
    {
        WorldPackets::Character::ChrCustomizationChoice& choice = createInfo.Customizations.emplace_back();
        choice.ChrCustomizationOptionID = optionId;
        choice.ChrCustomizationChoiceID = choiceId;
    }

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
    sCharacterCache->AddCharacterCacheEntry(guid, accountId, name, createInfo.Sex, race, classId, newChar->GetLevel(), false);

    newChar->CleanupsBeforeDelete();
    delete newChar;

    TC_LOG_INFO("misc", "ForeverBots: created bot character {} ({}) race {} class {}", name, guid.ToString(), race, classId);
    return guid;
}

ObjectGuid ForeverBotMgr::FindOrCreateCharacter(Player* owner, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
    CharacterDatabaseTransaction& pendingCommit)
{
    return FindOrCreateCharacter(owner->GetTeamId() == TEAM_HORDE ? TEAM_HORDE : TEAM_ALLIANCE, classId, accountId, session, error, pendingCommit);
}

ObjectGuid ForeverBotMgr::FindOrCreateCharacter(uint8 team, uint8 classId, uint32 accountId, WorldSession* session, std::string& error,
    CharacterDatabaseTransaction& pendingCommit)
{
    bool const horde = team == TEAM_HORDE;
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

    return CreateCharacter(Trinity::Containers::SelectRandomContainerElement(races), classId, accountId, session, error, pendingCommit);
}

void ForeverBotMgr::EnsureCustomizations(ObjectGuid guid, WorldSession* session)
{
    QueryResult info = CharacterDatabase.Query(Trinity::StringFormat("SELECT race, gender, class FROM characters WHERE guid = {}", guid.GetCounter()).c_str());
    if (!info)
        return;
    uint8 const race = (*info)[0].GetUInt8();
    uint8 const sex = (*info)[1].GetUInt8();
    uint8 const classId = (*info)[2].GetUInt8();

    // keep the current look if the server accepts it (Player::LoadFromDB refuses the whole character otherwise)
    std::vector<UF::ChrCustomizationChoice> current;
    if (QueryResult rows = CharacterDatabase.Query(Trinity::StringFormat(
        "SELECT chrCustomizationOptionID, chrCustomizationChoiceID FROM character_customizations WHERE guid = {} ORDER BY chrCustomizationOptionID", guid.GetCounter()).c_str()))
    {
        do
        {
            UF::ChrCustomizationChoice& choice = current.emplace_back();
            choice.ChrCustomizationOptionID = (*rows)[0].GetUInt32();
            choice.ChrCustomizationChoiceID = (*rows)[1].GetUInt32();
        } while (rows->NextRow());
    }
    // ... and the Classic character creator offers it (see IsFreeCustomizationReq)
    bool const offered = std::ranges::all_of(current, [&](UF::ChrCustomizationChoice const& choice)
    {
        ChrCustomizationOptionEntry const* option = sChrCustomizationOptionStore.LookupEntry(choice.ChrCustomizationOptionID);
        if (!option || !IsFreeCustomizationReq(option->ChrCustomizationReqID, race, classId))
            return false;
        std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
        if (!choices)
            return false;
        auto itr = std::ranges::find(*choices, uint32(choice.ChrCustomizationChoiceID), &ChrCustomizationChoiceEntry::ID);
        return itr != choices->end() && IsFreeCustomizationReq((*itr)->ChrCustomizationReqID, race, classId);
    });
    if (!current.empty() && offered && session->ValidateAppearance(Races(race), Classes(classId), Gender(sex), { current.data(), current.data() + current.size() }))
        return;

    // none (bots made before the appearance fix) or invalid ones: a new valid look. Async like the position update:
    // the login query runs after these (and after the save of a bot that just logged out), the character database
    // has a single async worker
    std::vector<std::pair<uint32, uint32>> customizations;
    PickValidCustomizations(session, race, classId, sex, customizations);
    CharacterDatabase.Execute(Trinity::StringFormat("DELETE FROM character_customizations WHERE guid = {}", guid.GetCounter()).c_str());
    for (auto const& [optionId, choiceId] : customizations)
        CharacterDatabase.Execute(Trinity::StringFormat("INSERT INTO character_customizations (guid, chrCustomizationOptionID, chrCustomizationChoiceID) VALUES ({}, {}, {})",
            guid.GetCounter(), optionId, choiceId).c_str());
}

void ForeverBotMgr::PlaceCharacterAt(ObjectGuid guid, uint32 mapId, Position const& pos)
{
    CharacterDatabase.Execute(Trinity::StringFormat(
        "UPDATE characters SET map = {}, instance_id = 0, position_x = {}, position_y = {}, position_z = {}, orientation = {} WHERE guid = {}",
        mapId, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation(), guid.GetCounter()).c_str());
}

void ForeverBotMgr::PlaceCharacterAt(ObjectGuid guid, Player* owner)
{
    // the bot logs in where the owner is (no teleport needed, works across maps and instances)
    Position pos = owner->GetNearPosition(2.0f, float(M_PI));
    CharacterDatabase.Execute(Trinity::StringFormat(
        "UPDATE characters SET map = {}, instance_id = {}, zone = {}, position_x = {}, position_y = {}, position_z = {}, orientation = {} WHERE guid = {}",
        owner->GetMapId(), owner->GetInstanceId(), owner->GetZoneId(), pos.GetPositionX(), pos.GetPositionY(), owner->GetPositionZ(), owner->GetOrientation(),
        guid.GetCounter()).c_str());
}

void ForeverBotMgr::LoginWhenSaved(ForeverBot& bot, CharacterDatabaseTransaction& pendingCommit, bool world)
{
    // new character: log in once it is saved (the callback runs from the session update, see Update)
    bot.Creating = true;
    bot.Session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(pendingCommit)).AfterComplete([this, guid = bot.Guid, world](bool success)
    {
        ForeverBot* created = GetBot(guid);
        if (!created)
            return;

        created->Creating = false;
        if (!success)
        {
            created->CreateFailed = true;
            return;
        }

        if (world)
            StartWorldLogin(*created);
        else if (Player* owner = ObjectAccessor::FindConnectedPlayer(created->Owner))
            StartLogin(*created, owner);
        else
            created->CreateFailed = true;
    });
}

std::string ForeverBotMgr::AddBot(Player* owner, uint8 classId, BotRole role, uint32 maxBots /*= 0*/)
{
    std::vector<ForeverBot*> current = GetBotsOf(owner->GetGUID());
    if (current.size() >= std::max(maxBots, MaxBotsPerOwner))
        return Trinity::StringFormat("You already have {} bots.", current.size());

    if (Group* group = owner->GetGroup())
        if (group->IsFull() && group->isRaidGroup())
            return "Your group is full.";

    uint32 accountId = GetBotAccountId();
    if (!accountId)
        return "Could not create the bot account.";

    WorldSession* session = CreateBotSession(accountId, PartyAccountName);
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

    if (pendingCommit)
        LoginWhenSaved(bot, pendingCommit, false);
    else
        StartLogin(bot, owner);
    return {};
}

void ForeverBotMgr::StartLogin(ForeverBot& bot, Player* owner)
{
    EnsureCustomizations(bot.Guid, bot.Session);
    PlaceCharacterAt(bot.Guid, owner);
    bot.Session->SetBotBeforeAddToMap([this, guid = bot.Guid](Player* botPlayer) { PrepareBeforeWorld(guid, botPlayer); });
    bot.Session->LoginBotCharacter(bot.Guid);
}

void ForeverBotMgr::StartWorldLogin(ForeverBot& bot)
{
    EnsureCustomizations(bot.Guid, bot.Session);
    bot.Session->SetBotBeforeAddToMap([this, guid = bot.Guid](Player* botPlayer) { PrepareBeforeWorld(guid, botPlayer); });
    bot.Session->LoginBotCharacter(bot.Guid);
}

void ForeverBotMgr::LoadClassicSpells()
{
    _trainerSpells.clear();
    _talentSpells.clear();
    _starterSpells.clear();
    _classBySpellFamily.clear();

    for (ChrClassesEntry const* classEntry : sChrClassesStore)
        if (classEntry->SpellClassSet)
            _classBySpellFamily[classEntry->SpellClassSet] = uint8(classEntry->ID);

    auto classOf = [this](SpellInfo const* info) -> uint8
    {
        auto itr = _classBySpellFamily.find(info->SpellFamilyName);
        return itr != _classBySpellFamily.end() ? itr->second : 0;
    };

    // what the class trainers teach (a trainer spell may be a "learn" spell: take what it teaches)
    if (QueryResult result = WorldDatabase.Query("SELECT ts.SpellId, MIN(ts.ReqLevel) FROM trainer_spell ts JOIN trainer t ON t.Id = ts.TrainerId WHERE t.Type = 0 GROUP BY ts.SpellId"))
    {
        do
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo((*result)[0].GetUInt32(), DIFFICULTY_NONE);
            uint8 const reqLevel = (*result)[1].GetUInt8();
            if (!info)
                continue;
            std::vector<uint32> taught;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                    taught.push_back(effect.TriggerSpell);
            if (taught.empty())
                taught.push_back(info->Id);
            for (uint32 spellId : taught)
                if (SpellInfo const* learned = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
                    if (uint8 classId = classOf(learned))
                    {
                        auto [itr, inserted] = _trainerSpells[classId].try_emplace(spellId, reqLevel);
                        if (!inserted)
                            itr->second = std::min(itr->second, reqLevel);
                    }
        } while (result->NextRow());
    }

    // class quest rewards (Defensive Stance, Bear Form, totems, demons...): allowed like the trainer spells
    if (QueryResult result = WorldDatabase.Query("SELECT RewardSpell FROM quest_template WHERE RewardSpell <> 0 UNION SELECT RewardDisplaySpell1 FROM quest_template WHERE RewardDisplaySpell1 <> 0"))
    {
        do
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo((*result)[0].GetUInt32(), DIFFICULTY_NONE);
            if (!info)
                continue;
            _talentSpells.insert(info->Id);
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                    _talentSpells.insert(effect.TriggerSpell);
        } while (result->NextRow());
    }

    // the class quest spells LearnClassSpells gives bots (whatever the quest data says)
    for (uint32 spellId : { 71u, 2458u, 5487u, 8071u, 3599u, 5394u, 688u, 697u, 712u, 691u, 7328u })
        _talentSpells.insert(spellId);

    // talents (trait definitions)
    for (TraitDefinitionEntry const* definition : sTraitDefinitionStore)
    {
        if (definition->SpellID)
            _talentSpells.insert(definition->SpellID);
        if (definition->VisibleSpellID)
            _talentSpells.insert(definition->VisibleSpellID);
    }

    // what a new character of the class knows at level 1 (learned with its skills)
    for (SkillLineAbilityEntry const* ability : sSkillLineAbilityStore)
    {
        if (ability->GetAcquireMethod() != SkillLineAbilityAcquireMethod::AutomaticSkillRank
            && ability->GetAcquireMethod() != SkillLineAbilityAcquireMethod::AutomaticCharLevel)
            continue;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell, DIFFICULTY_NONE); info && info->SpellLevel <= 1 && ability->MinSkillLineRank <= 1)
            _starterSpells.insert(info->Id);
    }

    size_t trainer = 0;
    for (auto const& [classId, spells] : _trainerSpells)
        trainer += spells.size();
    TC_LOG_INFO("server.loading", "ForeverBots: Classic class spells: {} from trainers ({} classes), {} talent spells, {} level 1 spells",
        trainer, _trainerSpells.size(), _talentSpells.size(), _starterSpells.size());
}

namespace
{
    // the spells a Classic character gets from its class quests (a bot doesn't do them). The tank ones matter most: the
    // warrior's Defensive Stance quest also teaches Taunt and Sunder Armor, the druid's Bear Form quest Growl and Maul;
    // without Taunt and Growl a tank bot never taunted (0 taunts in every test run) and mobs stayed on the damage dealers.
    struct ClassQuestSpell { uint8 ClassId; uint8 Level; uint32 SpellId; };
    ClassQuestSpell const ClassQuestSpells[] =
    {
        { CLASS_WARRIOR, 10, 71 },      // Defensive Stance
        { CLASS_WARRIOR, 10, 355 },     // Taunt
        { CLASS_WARRIOR, 30, 2458 },    // Berserker Stance
        { CLASS_DRUID,   10, 5487 },    // Bear Form
        { CLASS_DRUID,   10, 6795 },    // Growl
        { CLASS_SHAMAN,   4, 8071 },    // Stoneskin Totem
        { CLASS_SHAMAN,  10, 3599 },    // Searing Totem
        { CLASS_SHAMAN,  20, 5394 },    // Healing Stream Totem
        { CLASS_WARLOCK,  1, 688 },     // Summon Imp
        { CLASS_WARLOCK, 10, 697 },     // Summon Voidwalker
        { CLASS_WARLOCK, 20, 712 },     // Summon Succubus
        { CLASS_WARLOCK, 30, 691 },     // Summon Felhunter
        { CLASS_PALADIN, 12, 7328 },    // Redemption
        { CLASS_HUNTER,  10, 883 },     // Call Pet
        { CLASS_HUNTER,  10, 982 },     // Revive Pet
    };
}

bool ForeverBotMgr::IsClassicSpell(uint8 classId, SpellInfo const* spellInfo) const
{
    auto family = _classBySpellFamily.find(spellInfo->SpellFamilyName);
    if (family == _classBySpellFamily.end() || family->second != classId)
        return true;            // not a class spell: racials, weapon skills, wand, generic spells
    for (ClassQuestSpell const& quest : ClassQuestSpells)
        if (quest.ClassId == classId && quest.SpellId == spellInfo->Id)
            return true;
    auto trainer = _trainerSpells.find(classId);
    if (trainer != _trainerSpells.end() && trainer->second.contains(spellInfo->Id))
        return true;
    return _talentSpells.contains(spellInfo->Id) || _starterSpells.contains(spellInfo->Id);
}

// spells a Classic character of this class can't have (learned by older bot code from the full client data)
void ForeverBotMgr::RemoveNonClassicSpells(Player* bot)
{
    std::vector<uint32> remove;
    for (auto const& [spellId, known] : bot->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.dependent)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (info && !IsClassicSpell(bot->GetClass(), info))
            remove.push_back(spellId);
    }
    std::string names;
    for (uint32 spellId : remove)
    {
        if (_debug)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE); info && info->SpellName)
                names += Trinity::StringFormat(" {}({})", (*info->SpellName)[LOCALE_enUS], spellId);
        bot->RemoveSpell(spellId, false, false);
    }
    if (!remove.empty() && _debug)
        TC_LOG_INFO("server.foreverbots", "ForeverBots: {} unlearned {} non-Classic spells:{}", bot->GetName(), remove.size(), names);
}

void ForeverBotMgr::LearnClassSpells(Player* bot)
{
    if (_trainerSpells.empty())
        LoadClassicSpells();
    RemoveNonClassicSpells(bot);

    for (ClassQuestSpell const& quest : ClassQuestSpells)
        if (quest.ClassId == bot->GetClass() && quest.Level <= bot->GetLevel() && !bot->HasSpell(quest.SpellId))
            if (sSpellMgr->GetSpellInfo(quest.SpellId, DIFFICULTY_NONE))
                bot->LearnSpell(quest.SpellId, false, 0, true);

    // the level 1 spells of the class (a new character has them; older bot code may have removed some)
    if (ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(bot->GetClass()))
        for (uint32 spellId : _starterSpells)
            if (!bot->HasSpell(spellId))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
                    info && info->SpellFamilyName == classEntry->SpellClassSet && !info->IsPassive() && bot->IsSpellFitByClassAndRace(spellId)
                    && SpellMgr::IsSpellValid(info, bot, false))
                    bot->LearnSpell(spellId, false, 0, true);

    // what the class trainers sell, up to the bot's level
    auto itr = _trainerSpells.find(bot->GetClass());
    if (itr == _trainerSpells.end())
        return;
    for (auto const& [spellId, reqLevel] : itr->second)
    {
        if (reqLevel > bot->GetLevel() || bot->HasSpell(spellId))
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!spellInfo || !bot->IsSpellFitByClassAndRace(spellId) || !SpellMgr::IsSpellValid(spellInfo, bot, false))
            continue;
        bot->LearnSpell(spellId, false, 0, true);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Gear: the best item the bot can use for each slot, up to a few levels above its own, from vanilla items only
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    struct GearPlan
    {
        bool TwoHand = false;       // 2H weapon in the main hand
        bool Shield = false;        // 1H + shield
        bool DualWield = false;     // 1H + 1H
        bool OffHandHoldable = false;
        bool Ranged = false;        // bow / gun / crossbow / thrown in the ranged slot
        bool Wand = false;
        std::vector<int32> Stats;   // preferred stats, best first
    };

    GearPlan PlanFor(uint8 classId, BotRole role)
    {
        GearPlan plan;
        switch (classId)
        {
            case CLASS_WARRIOR:
                plan.Shield = role == BotRole::Tank; plan.TwoHand = role != BotRole::Tank; plan.Ranged = true;
                plan.Stats = { MOD_STRENGTH, MOD_STAMINA, MOD_AGILITY };
                break;
            case CLASS_PALADIN:
                plan.Shield = role != BotRole::Dps; plan.TwoHand = role == BotRole::Dps;
                plan.Stats = role == BotRole::Heal ? std::vector<int32>{ MOD_INTELLECT, MOD_SPIRIT, MOD_STAMINA } : std::vector<int32>{ MOD_STRENGTH, MOD_STAMINA, MOD_INTELLECT };
                break;
            case CLASS_HUNTER:
                plan.TwoHand = true; plan.Ranged = true;
                plan.Stats = { MOD_AGILITY, MOD_STAMINA, MOD_INTELLECT };
                break;
            case CLASS_ROGUE:
                plan.DualWield = true; plan.Ranged = true;
                plan.Stats = { MOD_AGILITY, MOD_STAMINA, MOD_STRENGTH };
                break;
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                plan.TwoHand = true; plan.Wand = true;
                plan.Stats = { MOD_INTELLECT, MOD_SPIRIT, MOD_STAMINA };
                break;
            case CLASS_SHAMAN:
                plan.Shield = true;
                plan.Stats = role == BotRole::Heal ? std::vector<int32>{ MOD_INTELLECT, MOD_SPIRIT, MOD_STAMINA } : std::vector<int32>{ MOD_INTELLECT, MOD_STRENGTH, MOD_STAMINA };
                break;
            case CLASS_DRUID:
                plan.TwoHand = true;
                plan.Stats = role == BotRole::Tank ? std::vector<int32>{ MOD_AGILITY, MOD_STAMINA, MOD_STRENGTH } : std::vector<int32>{ MOD_INTELLECT, MOD_SPIRIT, MOD_STAMINA };
                break;
            default:
                plan.TwoHand = true;
                break;
        }
        return plan;
    }

    bool IsPrimaryStat(int32 stat) { return stat == MOD_AGILITY || stat == MOD_STRENGTH || stat == MOD_INTELLECT || stat == MOD_SPIRIT; }

    int32 ScoreItem(ItemTemplate const& proto, GearPlan const& plan)
    {
        int32 score = int32(proto.GetBaseItemLevel()) * 10 + int32(proto.GetQuality()) * 4;
        for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            int32 stat = proto.GetStatModifierBonusStat(i);
            if (stat < 0)
                continue;
            auto itr = std::ranges::find(plan.Stats, stat);
            if (itr != plan.Stats.end())
                score += 12 - int32(std::distance(plan.Stats.begin(), itr)) * 3;
            else if (IsPrimaryStat(stat))
                score -= 10;                       // e.g. intellect on a warrior
        }
        // a weapon is its damage first: by item level and stats alone a warrior and a rogue ended up with the same dagger
        // (18-34 damage at level 22) and hit for little. Casters keep choosing by stats (their staff or wand damage is
        // irrelevant); a slower weapon hits harder with instant strikes.
        if (proto.GetClass() == ITEM_CLASS_WEAPON && !plan.Stats.empty() && plan.Stats.front() != MOD_INTELLECT)
        {
            score += int32(proto.GetDPS(proto.GetBaseItemLevel()) * 12.0f);
            score += int32(proto.GetDelay() / 250);
        }
        // heavier armor is better for whoever can wear it (cloaks are always cloth)
        if (proto.GetClass() == ITEM_CLASS_ARMOR && proto.GetInventoryType() != INVTYPE_CLOAK)
            score += int32(proto.GetSubClass()) * 6;
        return score;
    }
}

void ForeverBotMgr::AutoGear(Player* bot, BotRole role)
{
    GearPlan const plan = PlanFor(bot->GetClass(), role);
    uint8 const level = bot->GetLevel();
    std::set<uint32> learnedSkills;

    // classic: rogues learn Dual Wield at 10, warriors and hunters at 20 (trainer spell 674)
    constexpr uint32 SpellDualWield = 674;
    if (plan.DualWield && !bot->HasSpell(SpellDualWield)
        && (bot->GetClass() == CLASS_ROGUE ? level >= 10 : level >= 20))
        bot->LearnSpell(SpellDualWield, false);

    std::unordered_map<uint8, std::vector<std::pair<int32, uint32>>> candidates;   // equipment slot -> (score, item)
    auto consider = [&](uint8 slot, ItemTemplate const& proto) { candidates[slot].emplace_back(ScoreItem(proto, plan), proto.GetId()); };

    for (auto const& [itemId, proto] : sObjectMgr->GetItemTemplateStore())
    {
        if (itemId >= MaxVanillaItemId)
            continue;
        if (proto.GetQuality() < ITEM_QUALITY_NORMAL || proto.GetQuality() > ITEM_QUALITY_RARE)
            continue;
        if (proto.GetClass() != ITEM_CLASS_WEAPON && proto.GetClass() != ITEM_CLASS_ARMOR)
            continue;
        if (proto.GetBaseItemLevel() > uint32(level) + 5u)
            continue;
        char const* name = proto.GetDefaultLocaleName();
        if (!name || !*name || strstr(name, "Monster") || strstr(name, "Deprecated") || strstr(name, "OLD") || strstr(name, "[PH]") || strstr(name, "Test"))
            continue;
        // weapon and armor skills: classic trainers sell them, a bot just learns what its class and race may use
        if (uint32 skill = proto.GetSkill(); skill && !bot->HasSkill(skill))
        {
            SkillRaceClassInfoEntry const* info = sDB2Manager.GetSkillRaceClassInfo(skill, bot->GetRace(), bot->GetClass());
            if (!info || info->MinLevel > level || !learnedSkills.insert(skill).second)
                continue;
            uint16 const maxValue = uint16(level) * 5;
            bot->SetSkill(skill, 1, maxValue, maxValue);
            if (!bot->HasSkill(skill))
                continue;
        }
        if (bot->CanUseItem(&proto) != EQUIP_ERR_OK)
            continue;

        uint32 const subClass = proto.GetSubClass();
        switch (proto.GetInventoryType())
        {
            case INVTYPE_HEAD:      break;      // Forever: no helmets for now, a helmet is the suspect of a client crash when gear is drawn (Forever.VisibleItemAppearance)
            case INVTYPE_NECK:      consider(EQUIPMENT_SLOT_NECK, proto); break;
            case INVTYPE_SHOULDERS: consider(EQUIPMENT_SLOT_SHOULDERS, proto); break;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:      consider(EQUIPMENT_SLOT_CHEST, proto); break;
            case INVTYPE_WAIST:     consider(EQUIPMENT_SLOT_WAIST, proto); break;
            case INVTYPE_LEGS:      consider(EQUIPMENT_SLOT_LEGS, proto); break;
            case INVTYPE_FEET:      consider(EQUIPMENT_SLOT_FEET, proto); break;
            case INVTYPE_WRISTS:    consider(EQUIPMENT_SLOT_WRISTS, proto); break;
            case INVTYPE_HANDS:     consider(EQUIPMENT_SLOT_HANDS, proto); break;
            case INVTYPE_FINGER:    consider(EQUIPMENT_SLOT_FINGER1, proto); break;
            case INVTYPE_CLOAK:     consider(EQUIPMENT_SLOT_BACK, proto); break;
            case INVTYPE_2HWEAPON:
                if (plan.TwoHand)
                    consider(EQUIPMENT_SLOT_MAINHAND, proto);
                break;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
                if (!plan.TwoHand)
                    consider(EQUIPMENT_SLOT_MAINHAND, proto);
                if (plan.DualWield && proto.GetInventoryType() == INVTYPE_WEAPON)
                    consider(EQUIPMENT_SLOT_OFFHAND, proto);
                break;
            case INVTYPE_WEAPONOFFHAND:
                if (plan.DualWield)
                    consider(EQUIPMENT_SLOT_OFFHAND, proto);
                break;
            case INVTYPE_SHIELD:
                if (plan.Shield)
                    consider(EQUIPMENT_SLOT_OFFHAND, proto);
                break;
            case INVTYPE_HOLDABLE:
                if (plan.OffHandHoldable)
                    consider(EQUIPMENT_SLOT_OFFHAND, proto);
                break;
            case INVTYPE_RANGED:
            case INVTYPE_THROWN:
                if (plan.Ranged)
                    consider(EQUIPMENT_SLOT_RANGED, proto);
                break;
            case INVTYPE_RANGEDRIGHT:
                if ((plan.Wand && subClass == ITEM_SUBCLASS_WEAPON_WAND) || (plan.Ranged && subClass != ITEM_SUBCLASS_WEAPON_WAND))
                    consider(EQUIPMENT_SLOT_RANGED, proto);
                break;
            default:
                break;
        }
    }

    // replace whatever is equipped (starting gear) with the best picks; second ring is the second best ring
    for (auto& [slot, list] : candidates)
        std::ranges::sort(list, std::greater<>());

    std::vector<std::pair<uint8, uint32>> picks;
    for (auto const& [slot, list] : candidates)
    {
        if (list.empty())
            continue;
        picks.emplace_back(slot, list[0].second);
        if (slot == EQUIPMENT_SLOT_FINGER1 && list.size() > 1)
            picks.emplace_back(EQUIPMENT_SLOT_FINGER2, list[1].second);
    }
    if (plan.DualWield)
    {
        // main hand and off hand picked from the same list could be the same unique item: take the next one then
        auto mh = std::ranges::find(picks, uint8(EQUIPMENT_SLOT_MAINHAND), &std::pair<uint8, uint32>::first);
        auto oh = std::ranges::find(picks, uint8(EQUIPMENT_SLOT_OFFHAND), &std::pair<uint8, uint32>::first);
        if (mh != picks.end() && oh != picks.end() && mh->second == oh->second && candidates[EQUIPMENT_SLOT_OFFHAND].size() > 1)
            oh->second = candidates[EQUIPMENT_SLOT_OFFHAND][1].second;
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD && slot != EQUIPMENT_SLOT_TRINKET1 && slot != EQUIPMENT_SLOT_TRINKET2)
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    // main hand first (a two-hander decides whether the off hand is free)
    std::ranges::sort(picks, [](auto const& a, auto const& b) { return (a.first == EQUIPMENT_SLOT_MAINHAND) > (b.first == EQUIPMENT_SLOT_MAINHAND); });
    for (auto const& [slot, itemId] : picks)
    {
        uint16 dest = 0;
        InventoryResult result = bot->CanEquipNewItem(slot, dest, itemId, false);
        if (result == EQUIP_ERR_OK)
            bot->EquipNewItem(dest, itemId, ItemContext::NONE, true);
        TC_LOG_DEBUG("misc", "ForeverBots: {} gear slot {} item {} ({} candidates) -> {}", bot->GetName(), slot, itemId,
            candidates[slot].size(), uint32(result));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Talents: Classic 1.60 talents are a trait config (no starter builds in its data). Points go one by one into the
// lowest row the trait validator accepts, the tab that fits the bot's role first.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
    // classic tab order (left to right): which tab a class/role wants
    uint8 PreferredTab(uint8 classId, BotRole role)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return role == BotRole::Tank ? 2 : 1;                          // protection / fury
            case CLASS_PALADIN: return role == BotRole::Heal ? 0 : role == BotRole::Tank ? 1 : 2;
            case CLASS_HUNTER:  return 0;                                                       // beast mastery
            case CLASS_ROGUE:   return 1;                                                       // combat
            case CLASS_PRIEST:  return role == BotRole::Heal ? 1 : 2;                          // holy / shadow
            case CLASS_SHAMAN:  return role == BotRole::Heal ? 2 : 0;                          // restoration / elemental
            case CLASS_MAGE:    return 2;                                                       // frost
            case CLASS_WARLOCK: return 0;                                                       // affliction
            case CLASS_DRUID:   return role == BotRole::Heal ? 2 : role == BotRole::Tank ? 1 : 0;
            default:            return 0;
        }
    }
}

void ForeverBotMgr::SpendTalents(Player* bot, BotRole role)
{
    if (bot->GetLevel() < 10)
        return;

    UF::TraitConfig const* active = bot->GetTraitConfig(bot->m_activePlayerData->ActiveCombatTraitConfigID);
    if (!active)
    {
        TC_LOG_INFO("server.foreverbots", "ForeverBots: {} has no active talent config ({})", bot->GetName(), int32(bot->m_activePlayerData->ActiveCombatTraitConfigID));
        return;
    }

    WorldPackets::Traits::TraitConfig config(*active);

    // the class talent trees: those of the class skill line of the bot's class (as TraitMgr finds them; the bot
    // doesn't always have that skill itself: mage, hunter, shaman and priest bots don't)
    std::set<int32> trees;
    uint32 const classMask = bot->GetClassMask();
    for (SkillLineXTraitTreeEntry const* link : sSkillLineXTraitTreeStore)
    {
        SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(link->SkillLineID);
        if (!skillLine || skillLine->CategoryID != SKILL_CATEGORY_CLASS)
            continue;
        for (SkillRaceClassInfoEntry const* info : sDB2Manager.GetSkillRaceClassInfo(skillLine->ID))
            if (info->ClassMask & classMask)
                trees.insert(link->TraitTreeID);
    }
    if (trees.empty())
    {
        TC_LOG_INFO("server.foreverbots", "ForeverBots: {} (class {}) has no talent tree skill line", bot->GetName(), bot->GetClass());
        return;
    }

    struct Candidate { TraitNodeEntry const* Node; int32 EntryId; int32 MaxRanks; int32 Tab; };
    std::vector<Candidate> candidates;
    for (TraitNodeEntry const* node : sTraitNodeStore)
    {
        if (!trees.contains(int32(node->TraitTreeID)))
            continue;
        TraitNodeXTraitNodeEntryEntry const* first = nullptr;
        for (TraitNodeXTraitNodeEntryEntry const* link : sTraitNodeXTraitNodeEntryStore)
            if (link->TraitNodeID == node->ID && (!first || link->Index < first->Index))
                first = link;
        if (!first)
            continue;
        TraitNodeEntryEntry const* entry = sTraitNodeEntryStore.LookupEntry(first->TraitNodeEntryID);
        if (!entry || entry->MaxRanks <= 0)
            continue;
        candidates.push_back({ node, first->TraitNodeEntryID, entry->MaxRanks, 0 });
    }
    if (candidates.empty())
        return;

    // three tabs: by sub tree when the data has them, otherwise by horizontal position
    std::map<int32, int32> tabKeys;     // key -> min PosX
    bool const bySubTree = std::ranges::any_of(candidates, [](Candidate const& c) { return c.Node->TraitSubTreeID != 0; });
    auto [minX, maxX] = std::ranges::minmax(candidates | std::views::transform([](Candidate const& c) { return c.Node->PosX; }));
    int32 const width = std::max(1, (maxX - minX + 1) / 3);
    for (Candidate& c : candidates)
    {
        int32 key = bySubTree ? c.Node->TraitSubTreeID : std::min(2, (c.Node->PosX - minX) / width);
        auto [itr, inserted] = tabKeys.try_emplace(key, c.Node->PosX);
        if (!inserted)
            itr->second = std::min(itr->second, c.Node->PosX);
        c.Tab = key;
    }
    std::vector<std::pair<int32, int32>> orderedTabs(tabKeys.begin(), tabKeys.end());   // (key, min x)
    std::ranges::sort(orderedTabs, {}, &std::pair<int32, int32>::second);
    uint8 const wanted = std::min<uint8>(PreferredTab(bot->GetClass(), role), uint8(orderedTabs.size() - 1));
    int32 const wantedKey = orderedTabs[wanted].first;

    std::ranges::sort(candidates, [&](Candidate const& a, Candidate const& b)
    {
        if ((a.Tab == wantedKey) != (b.Tab == wantedKey))
            return a.Tab == wantedKey;
        if (a.Node->PosY != b.Node->PosY)
            return a.Node->PosY < b.Node->PosY;
        return a.Node->PosX < b.Node->PosX;
    });

    bool changed = false;
    int32 spentPoints = 0;
    std::map<int32, int32> lastFailures;   // LearnResult -> count, on the pass that stopped
    for (int32 points = 0; points < 70; ++points)
    {
        bool spent = false;
        lastFailures.clear();
        for (Candidate const& c : candidates)
        {
            WorldPackets::Traits::TraitConfig attempt = config;
            auto itr = std::ranges::find_if(attempt.Entries, [&](WorldPackets::Traits::TraitEntry const& e) { return e.TraitNodeID == int32(c.Node->ID); });
            if (itr != attempt.Entries.end())
            {
                if (itr->TraitNodeEntryID != c.EntryId || itr->Rank + itr->GrantedRanks >= c.MaxRanks)
                    continue;
                ++itr->Rank;
            }
            else
            {
                WorldPackets::Traits::TraitEntry& added = attempt.Entries.emplace_back();
                added.TraitNodeID = c.Node->ID;
                added.TraitNodeEntryID = c.EntryId;
                added.Rank = 1;
            }

            TraitMgr::LearnResult result = TraitMgr::ValidateConfig(attempt, bot);
            if (result == TraitMgr::LearnResult::Ok)
            {
                config = std::move(attempt);
                spent = changed = true;
                ++spentPoints;
                break;
            }
            ++lastFailures[AsUnderlyingType(result)];
        }
        if (!spent)
            break;
    }

    std::string failures;
    for (auto const& [result, count] : lastFailures)
        failures += Trinity::StringFormat(" {}x{}", result, count);
    TC_LOG_INFO("server.foreverbots", "ForeverBots: {} (class {}, level {}) talents: {} trees, {} nodes, {} tabs (wanted {}), {} points spent now, {} entries; stopped on:{}",
        bot->GetName(), bot->GetClass(), bot->GetLevel(), trees.size(), candidates.size(), orderedTabs.size(), wanted, spentPoints, config.Entries.size(), failures);

    if (changed)
        bot->UpdateTraitConfig(std::move(config), 0, false);
}

// The Classic combat formulas use the vanilla weapon and defense skills (hit, miss, crit, glancing), which grow with use.
// A bot jumps levels at its logins: give it the skill a character of its level would have trained (level x 5),
// otherwise it misses almost every swing (a level 36 warrior was at 22 in two-handed axes).
void ForeverBotMgr::MaxCombatSkills(Player* bot)
{
    static uint32 const CombatSkills[] = { 43 /*swords*/, 44 /*axes*/, 45 /*bows*/, 46 /*guns*/, 54 /*maces*/, 55 /*2h swords*/,
        95 /*defense*/, 136 /*staves*/, 160 /*2h maces*/, 162 /*unarmed*/, 172 /*2h axes*/, 173 /*daggers*/, 176 /*thrown*/,
        226 /*crossbows*/, 228 /*wands*/, 229 /*polearms*/, 473 /*fist weapons*/ };
    uint16 const max = uint16(bot->GetLevel() * 5);
    for (uint32 skill : CombatSkills)
        if (bot->HasSkill(skill) && (bot->GetPureSkillValue(skill) < max || bot->GetPureMaxSkillValue(skill) < max))
            bot->SetSkill(skill, bot->GetSkillStep(skill), max, max);
}

// Runs inside the bot's login, after it is loaded and before it is added to the map: nobody sees it yet, so the
// level, the spells, the gear and the group membership reach the other players in the create object. Joining a
// group while visible sends a full party member values update, which crashes the 1.60 client.
void ForeverBotMgr::PrepareBeforeWorld(ObjectGuid guid, Player* botPlayer)
{
    ForeverBot* bot = GetBot(guid);
    if (!bot)
        return;

    // a player sees nothing (Player::CanNeverSee) until its client reports the end of loading
    // (CMSG_MOVE_INIT_ACTIVE_MOVER_COMPLETE / time sync): without this flag a bot can't find or even fight back
    botPlayer->SetPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME);
    botPlayer->SetTransportServerTime(0);

    Player* owner = bot->IsWorldBot() ? nullptr : ObjectAccessor::FindConnectedPlayer(bot->Owner);
    uint8 const targetLevel = owner ? owner->GetLevel() : (bot->WorldLevel ? bot->WorldLevel : botPlayer->GetLevel());
    bot->WorldLevel = 0;

    if (botPlayer->GetLevel() != targetLevel)
        botPlayer->GiveLevel(targetLevel);
    LearnClassSpells(botPlayer);
    SpendTalents(botPlayer, bot->Role);
    AutoGear(botPlayer, bot->Role);
    MaxCombatSkills(botPlayer);
    if (botPlayer->isDead())
        botPlayer->ResurrectPlayer(1.0f);
    botPlayer->SetFullHealth();
    botPlayer->SetFullPower(botPlayer->GetPowerType());

    if (!owner)
        return;

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
}

void ForeverBotMgr::InitializeInWorld(ForeverBot& bot, Player* botPlayer, Player* owner)
{
    if (!botPlayer->AI())
        botPlayer->SetBotAI(new ForeverBotAI(botPlayer));

    if (bot.IsWorldBot())
        bot.Home = botPlayer->GetPosition();
    bot.LoginLevel = botPlayer->GetLevel();

    bool const announce = !bot.Relogging;
    bot.Ready = true;
    bot.Relogging = false;
    if (owner && announce)
        ChatHandler(owner->GetSession()).SendSysMessage(Trinity::StringFormat("ForeverBots: {} joins as {}.", botPlayer->GetName(), BotRoleName(bot.Role)));
}

void ForeverBotMgr::Teleport(ForeverBot& bot, Player* botPlayer, Player* owner)
{
    if (botPlayer->GetMapId() != owner->GetMapId())
        return;

    // rings of eight behind and around the owner (a raid has up to 39 bots)
    Position pos = owner->GetNearPosition(2.0f + (bot.Index % 8) * 0.5f + (bot.Index / 8) * 2.0f, float(M_PI) - 0.6f + bot.Index * 0.4f);
    pos.m_positionZ = owner->GetPositionZ();
    botPlayer->GetMotionMaster()->Clear();
    botPlayer->NearTeleportTo(pos);
    bot.Session->FinishBotTeleport();
}

// Logs the bot out and back in: used for map changes and level ups, so every big change happens in
// PrepareBeforeWorld while nobody sees the bot
void ForeverBotMgr::Relog(ForeverBot& bot, Player* owner)
{
    Logout(bot);
    bot.Session = CreateBotSession(bot.IsWorldBot() ? GetWorldAccountId() : GetBotAccountId(), bot.IsWorldBot() ? WorldAccountName : PartyAccountName);
    bot.Relogging = true;
    if (bot.IsWorldBot())
        StartWorldLogin(bot);
    else
        StartLogin(bot, owner);
}

void ForeverBotMgr::Logout(ForeverBot& bot)
{
    if (!bot.Session)
        return;

    // a dead bot would be sent to a graveyard while logging out: just stand it back up
    if (Player* botPlayer = bot.Session->GetPlayer())
    {
        if (botPlayer->isDead())
        {
            botPlayer->ResurrectPlayer(1.0f);
            botPlayer->SpawnCorpseBones();
        }
        if (botPlayer->IsBeingTeleported())
            bot.Session->FinishBotTeleport();
    }

    TC_LOG_DEBUG("misc", "ForeverBots: logging out {}", bot.Guid.ToString());
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

    // leave the world first, then the group: the owner's client then never gets a party values update about a bot
    // it can still see
    Group* group = nullptr;
    if (itr->second.Session)
        if (Player* botPlayer = itr->second.Session->GetPlayer())
            group = botPlayer->GetGroup();

    Logout(itr->second);
    if (group && group->IsMember(guid))
        group->RemoveMember(guid);
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
    if (!guids.empty())
        TC_LOG_INFO("misc", "ForeverBots: logging out {} bots", guids.size());
    for (ObjectGuid guid : guids)
        RemoveBot(guid);
    if (!guids.empty())
        TC_LOG_INFO("misc", "ForeverBots: all bots logged out");
}

std::vector<ForeverBot*> ForeverBotMgr::GetBotsOf(ObjectGuid owner)
{
    std::vector<ForeverBot*> result;
    for (auto& [guid, bot] : _bots)
        if (!bot.IsWorldBot() && bot.Owner == owner)
            result.push_back(&bot);
    return result;
}

ForeverBot* ForeverBotMgr::GetBot(ObjectGuid guid)
{
    auto itr = _bots.find(guid);
    return itr != _bots.end() ? &itr->second : nullptr;
}

std::vector<std::string> ForeverBotMgr::DescribeBots()
{
    std::vector<std::string> lines;
    for (auto const& [guid, bot] : _bots)
    {
        Player* p = bot.Session ? bot.Session->GetPlayer() : nullptr;
        if (!p || !p->IsInWorld())
        {
            lines.push_back(Trinity::StringFormat("{} {}: not in world (creating {}, loading {}, relogging {})", guid.ToString(), bot.IsWorldBot() ? "world" : "party",
                bot.Creating, bot.Session && bot.Session->PlayerLoading(), bot.Relogging));
            continue;
        }
        Unit* victim = p->GetVictim();
        std::string attackers;
        for (Unit* attacker : p->getAttackers())
            attackers += Trinity::StringFormat(" [{} lvl {} valid {} dist {:.0f} see {} unattackable {} flags {:#x} immunePC {} meImmuneNPC {} uninteractible {} uber {}]",
                attacker->GetName(), attacker->GetLevel(), p->IsValidAttackTarget(attacker), p->GetDistance(attacker), p->CanSeeOrDetect(attacker),
                attacker->HasUnitState(UNIT_STATE_UNATTACKABLE), uint32(*attacker->m_unitData->Flags), attacker->IsImmuneToPC(), p->IsImmuneToNPC(),
                attacker->IsUninteractible(), p->HasPlayerFlag(PLAYER_FLAGS_UBER));
        std::string threats;
        for (auto const& [threatGuid, ref] : p->GetCombatManager().GetPvECombatRefs())
            if (Unit* other = ref->GetOther(p))
                threats += Trinity::StringFormat(" [{} valid {} victim {}]", other->GetName(), p->IsValidAttackTarget(other), other->GetVictim() ? other->GetVictim()->GetName() : "-");
        lines.push_back(Trinity::StringFormat("  attackers:{} combat refs:{}", attackers.empty() ? " none" : attackers, threats.empty() ? " none" : threats));
        Item const* mainHand = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item const* ranged = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        lines.push_back(Trinity::StringFormat("  victim hp {:.0f}% dist {:.1f} meleeAttacking {} attackTimer {} dmg {}-{} | last target {} attack ok {} | mounted {} pacified {} | last cast {} result {} | main hand {} ranged {} | autorepeat {} | meleeRange {} | facing {}",
            victim ? victim->GetHealthPct() : 0.0f, victim ? p->GetDistance(victim) : 0.0f, p->HasUnitState(UNIT_STATE_MELEE_ATTACKING), p->getAttackTimer(BASE_ATTACK),
            p->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE), p->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE),
            bot.LastTarget, bot.LastAttackOk, p->IsMounted(), p->HasUnitFlag(UNIT_FLAG_PACIFIED),
            bot.LastCastSpell, bot.LastCastResult, mainHand ? mainHand->GetEntry() : 0, ranged ? ranged->GetEntry() : 0,
            p->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr, victim && p->IsWithinMeleeRange(victim), victim && p->HasInArc(float(M_PI) / 2, victim)));
        lines.push_back(Trinity::StringFormat("{} {} lvl {} xp {} kills {} {} map {} ({:.0f}, {:.0f}, {:.0f}) home dist {:.0f} | ai {} ready {} | {}{} victim {} | motion {} | hp {:.0f}% power {:.0f}%",
            p->GetName(), bot.IsWorldBot() ? "world" : "party", p->GetLevel(), *p->m_activePlayerData->XP, bot.Kills, p->IsAlive() ? "alive" : "dead",
            p->GetMapId(), p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), bot.IsWorldBot() ? bot.Home.GetExactDist2d(p) : 0.0f,
            p->AI() != nullptr, bot.Ready, p->IsInCombat() ? "combat " : "", p->isMoving() ? "moving" : "still",
            victim ? victim->GetName() : "-", uint32(p->GetMotionMaster()->GetCurrentMovementGeneratorType()), p->GetHealthPct(),
            p->GetPowerType() == POWER_MANA ? p->GetPowerPct(POWER_MANA) : 0.0f));
    }
    if (lines.empty())
        lines.push_back("ForeverBots: no bots.");
    return lines;
}

uint32 ForeverBotMgr::CountWorldBots() const
{
    uint32 count = 0;
    for (auto const& [guid, bot] : _bots)
        if (bot.IsWorldBot())
            ++count;
    return count;
}

void ForeverBotMgr::SetWorldBotCount(uint32 count, bool persist /*= true*/)
{
    _worldTarget = std::min(count, MaxWorldBots);
    _balanceTimer = 0;

    // kept across restarts (world_variable of this realm's characters database)
    if (persist)
        CharacterDatabase.Execute(Trinity::StringFormat("REPLACE INTO world_variable (ID, Value) VALUES ('ForeverBots.WorldBots', {})", _worldTarget).c_str());
}

void ForeverBotMgr::LoadWorldBotCount(uint32 configDefault)
{
    uint32 count = configDefault;
    if (QueryResult result = CharacterDatabase.Query("SELECT Value FROM world_variable WHERE ID = 'ForeverBots.WorldBots'"))
        count = uint32(std::max<int32>(0, (*result)[0].GetInt32()));
    SetWorldBotCount(count, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// World bots
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotMgr::BuildWorldSpots()
{
    // hunting spots: where creatures with a vanilla level live on the two continents (not vendors / quest givers)
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapId != 0 && data.mapId != 1)
            continue;
        std::pair<uint8, uint8> const* level = sObjectMgr->GetCreatureClassicLevel(data.id);
        if (!level || level->second > 60)
            continue;
        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creatureTemplate || creatureTemplate->npcflag || creatureTemplate->Classification != CreatureClassifications::Normal)
            continue;
        // monsters only: hostile to players or neutral to all (no guards / town folk of either faction)
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
        if (!faction || !(faction->IsHostileToPlayers() || faction->IsNeutralToAll()))
            continue;

        WorldSpot& spot = _worldSpots.emplace_back();
        spot.MapId = uint16(data.mapId);
        spot.MinLevel = level->first;
        spot.MaxLevel = level->second;
        spot.Pos = data.spawnPoint;
    }
    TC_LOG_INFO("misc", "ForeverBots: {} world bot hunting spots", _worldSpots.size());
}

bool ForeverBotMgr::PickWorldSpot(uint8 level, uint32& mapId, Position& pos)
{
    if (_worldSpots.empty())
        BuildWorldSpots();

    std::vector<WorldSpot const*> matching;
    for (WorldSpot const& spot : _worldSpots)
        if (spot.MinLevel <= level + 1 && spot.MaxLevel + 1 >= level)
            matching.push_back(&spot);
    if (matching.empty())
        return false;

    WorldSpot const* spot = Trinity::Containers::SelectRandomContainerElement(matching);
    mapId = spot->MapId;
    pos = spot->Pos;
    pos.m_positionX += frand(-6.0f, 6.0f);
    pos.m_positionY += frand(-6.0f, 6.0f);
    return true;
}

void ForeverBotMgr::BalanceWorldBots()
{
    uint32 online = CountWorldBots();

    // too many: log the newest out
    while (online > _worldTarget)
    {
        ObjectGuid victim;
        for (auto const& [guid, bot] : _bots)
            if (bot.IsWorldBot())
                victim = guid;
        if (victim.IsEmpty())
            break;
        RemoveBot(victim);
        --online;
    }

    if (online >= _worldTarget)
        return;

    uint32 accountId = GetWorldAccountId();
    if (!accountId)
        return;

    uint32 logins = 0;

    // first the world bots that already exist, each at a fresh hunting spot of its level
    if (QueryResult result = CharacterDatabase.Query(Trinity::StringFormat("SELECT guid, class, level FROM characters WHERE account = {} AND deleteDate IS NULL ORDER BY RAND()", accountId).c_str()))
    {
        do
        {
            if (online >= _worldTarget || logins >= MaxLoginsPerBalance)
                return;
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>((*result)[0].GetUInt64());
            if (_bots.contains(guid))
                continue;

            uint8 classId = (*result)[1].GetUInt8();
            uint32 spotMap = 0;
            Position spot;
            if (PickWorldSpot((*result)[2].GetUInt8(), spotMap, spot))
                PlaceCharacterAt(guid, spotMap, spot);

            ForeverBot& bot = _bots[guid];
            bot.Guid = guid;
            bot.World = true;
            bot.Role = classId == CLASS_PRIEST ? BotRole::Heal : BotRole::Dps;
            bot.Session = CreateBotSession(accountId, WorldAccountName);
            StartWorldLogin(bot);
            ++online;
            ++logins;
        } while (result->NextRow());
    }

    // then new ones, of a random class, faction and level, at a hunting spot of their level
    while (online < _worldTarget && logins < MaxLoginsPerBalance)
    {
        uint8 classId = Trinity::Containers::SelectRandomContainerElement(BotClasses);
        bool horde = urand(0, 1) == 1;
        std::vector<uint8> races;
        for (uint8 race : horde ? std::span<uint8 const>(HordeRaces) : std::span<uint8 const>(AllianceRaces))
            if (sObjectMgr->GetPlayerInfo(race, classId))
                races.push_back(race);
        if (races.empty())
            continue;

        uint8 level = uint8(urand(1, 100) <= 60 ? urand(1, 30) : urand(31, 60));
        uint32 mapId = 0;
        Position pos;
        if (!PickWorldSpot(level, mapId, pos))
            return;

        WorldSession* session = CreateBotSession(accountId, WorldAccountName);
        std::string error;
        CharacterDatabaseTransaction pendingCommit;
        ObjectGuid guid = CreateCharacter(Trinity::Containers::SelectRandomContainerElement(races), classId, accountId, session, error, pendingCommit);
        if (guid.IsEmpty())
        {
            delete session;
            TC_LOG_ERROR("misc", "ForeverBots: world bot creation failed: {}", error);
            return;
        }

        ForeverBot& bot = _bots[guid];
        bot.Guid = guid;
        bot.World = true;
        bot.Role = classId == CLASS_PRIEST ? BotRole::Heal : BotRole::Dps;
        bot.WorldLevel = level;
        bot.Session = session;

        // the new character is saved first; its position is set once the save is done, right before the login
        bot.Creating = true;
        session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(pendingCommit)).AfterComplete([this, guid, mapId, pos](bool success)
        {
            ForeverBot* created = GetBot(guid);
            if (!created)
                return;
            created->Creating = false;
            if (!success)
            {
                created->CreateFailed = true;
                return;
            }
            PlaceCharacterAt(guid, mapId, pos);
            StartWorldLogin(*created);
        });

        ++online;
        ++logins;
    }
}

void ForeverBotMgr::UpdateWorldBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& /*toRemove*/)
{
    Player* botPlayer = bot.Session->GetPlayer();

    if (botPlayer->isDead())
    {
        bot.DeadTimer += diff;
        if (bot.DeadTimer >= WorldResurrectDelay)
        {
            botPlayer->ResurrectPlayer(0.7f);
            botPlayer->SpawnCorpseBones();
            bot.DeadTimer = 0;
        }
        return;
    }
    bot.DeadTimer = 0;

    bot.RelogCheckTimer += diff;
    if (bot.RelogCheckTimer < 10000)
        return;
    bot.RelogCheckTimer = 0;

    // progress watchdog: no experience for a few minutes (nothing to hunt around, a target it can't reach...)
    // -> another hunting spot of its level
    uint32 const xp = *botPlayer->m_activePlayerData->XP;
    if (xp != bot.LastXp || botPlayer->GetLevel() != bot.LoginLevel)
    {
        bot.LastXp = xp;
        bot.StuckTime = 0;
    }
    else if ((bot.StuckTime += 10000) >= 180000)
    {
        bot.StuckTime = 0;
        uint32 mapId = 0;
        Position pos;
        if (PickWorldSpot(botPlayer->GetLevel(), mapId, pos))
        {
            TC_LOG_DEBUG("misc", "ForeverBots: {} made no progress, moving to another hunting spot", botPlayer->GetName());
            Logout(bot);
            PlaceCharacterAt(bot.Guid, mapId, pos);
            bot.Session = CreateBotSession(GetWorldAccountId(), WorldAccountName);
            bot.Relogging = true;
            StartWorldLogin(bot);
            return;
        }
    }

    if (botPlayer->IsInCombat())
        return;

    // it leveled up from its own kills: a relog teaches the new spells and finds better gear, out of sight;
    // every few levels it also moves on to a hunting spot of its new level
    if (botPlayer->GetLevel() > bot.LoginLevel)
    {
        uint8 const level = botPlayer->GetLevel();
        if (level >= bot.LoginLevel + 3)
        {
            uint32 mapId = 0;
            Position pos;
            if (PickWorldSpot(level, mapId, pos))
            {
                Logout(bot);
                PlaceCharacterAt(bot.Guid, mapId, pos);
                bot.Session = CreateBotSession(GetWorldAccountId(), WorldAccountName);
                bot.Relogging = true;
                StartWorldLogin(bot);
                return;
            }
        }
        Relog(bot, nullptr);
    }
}

void ForeverBotMgr::UpdatePartyBot(ForeverBot& bot, uint32 diff, std::vector<ObjectGuid>& toRemove)
{
    Player* owner = ObjectAccessor::FindConnectedPlayer(bot.Owner);
    if (!owner)
    {
        toRemove.push_back(bot.Guid);
        return;
    }

    Player* botPlayer = bot.Session->GetPlayer();
    if (!owner->IsInWorld() || owner->IsBeingTeleported())
        return;

    // the owner went to another map or instance: log the bot out and back in at the owner's position
    if (botPlayer->GetMapId() != owner->GetMapId() || botPlayer->GetInstanceId() != owner->GetInstanceId())
    {
        Relog(bot, owner);
        return;
    }

    if (botPlayer->isDead())
    {
        if (!bot.DeadTimer)
            ++bot.Deaths;
        bot.DeadTimer += diff;
        if (bot.DeadTimer >= ResurrectDelay && owner->IsAlive() && !owner->IsInCombat())
        {
            botPlayer->ResurrectPlayer(0.5f);
            botPlayer->SpawnCorpseBones();
            Teleport(bot, botPlayer, owner);
            bot.DeadTimer = 0;
        }
        return;
    }
    bot.DeadTimer = 0;

    // the owner leveled up: catch up (level, spells, gear) through a relog, out of sight
    if (botPlayer->GetLevel() < owner->GetLevel() && !botPlayer->IsInCombat() && !owner->IsInCombat())
    {
        Relog(bot, owner);
        return;
    }

    // bots have no mounts: while the owner rides, they catch up much sooner
    float const regroup = owner->IsMounted() ? 40.0f : RegroupDistance;
    if (bot.SummonRequested || (!botPlayer->IsInCombat() && !owner->IsInCombat() && !botPlayer->IsWithinDistInMap(owner, regroup)))
    {
        bot.SummonRequested = false;
        Teleport(bot, botPlayer, owner);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Group Finder runs: the player posts a group for a dungeon in the game's own window, the bots fill it
// ---------------------------------------------------------------------------------------------------------------------
bool ForeverBotMgr::StartDungeonRun(Player* owner, uint32 category, std::vector<uint32> const& activities, uint8 roles)
{
    // GroupFinderCategory 2 = Dungeons, 114 = Raids (116 quests and zones, 118 battlegrounds, 120 custom)
    if ((category != 2 && category != 114) || activities.empty() || IsBot(owner->GetGUID()))
        return false;

    // the first ticked activity that is known (world.forever_groupfinder_dungeon, read here: it is tiny and rare)
    std::string ids;
    for (uint32 activity : activities)
        ids += (ids.empty() ? "" : ",") + std::to_string(activity);
    QueryResult result = WorldDatabase.Query(Trinity::StringFormat(
        "SELECT ActivityID, PortLocID, Name, MaxPlayers FROM forever_groupfinder_dungeon WHERE ActivityID IN ({}) ORDER BY FIELD(ActivityID, {}) LIMIT 1", ids, ids).c_str());
    if (!result)
    {
        // e.g. the new Forever dungeons, still empty on the server: say so instead of leaving the player waiting
        ChatHandler(owner->GetSession()).SendSysMessage("ForeverBots: no bot run for this dungeon or raid yet. Your group stays listed; pick another one for a run with bots.");
        return false;
    }

    Field* fields = result->Fetch();
    std::string const name = fields[2].GetString();
    WorldSafeLocsEntry const* entrance = sObjectMgr->GetWorldSafeLoc(fields[1].GetUInt32());
    if (!entrance)
    {
        TC_LOG_ERROR("misc", "ForeverBots: Group Finder activity {} has no world_safe_locs {}", fields[0].GetUInt32(), fields[1].GetUInt32());
        return false;
    }
    // ForeverBots.GroupFinder.MaxGroupSize: a server that cannot afford 39 bots per player runs the raids with smaller groups
    uint32 const maxSize = std::clamp<uint32>(sConfigMgr->GetIntDefault("ForeverBots.GroupFinder.MaxGroupSize", 40), 5, 40);
    uint8 const size = uint8(std::clamp<uint32>(fields[3].GetUInt8(), 5, maxSize));

    ChatHandler chat(owner->GetSession());
    Group const* group = owner->GetGroup();
    if (group && group->GetLeaderGUID() != owner->GetGUID())
    {
        chat.SendSysMessage("ForeverBots: only the group leader can start a run.");
        return false;
    }
    if (group && group->isRaidGroup() && size <= 5)
    {
        chat.SendSysMessage("ForeverBots: a raid group cannot enter this dungeon. Leave the raid and post the group again.");
        return false;
    }
    if (owner->IsInCombat() || !owner->IsAlive() || owner->IsInFlight())
    {
        chat.SendSysMessage("ForeverBots: you cannot start a run now (in combat, dead or flying). Post the group again afterwards.");
        return false;
    }

    // The teleport waits for UpdateDungeonRuns: right after posting, the client searches the listings (CMSG_LFG_LIST_SEARCH)
    // and waits for the results forever ("Searching..." in the window, also in later sessions of the window). A far teleport
    // from here made the server drop that search.
    DungeonRun& run = _dungeonRuns[owner->GetGUID()];
    run = DungeonRun();
    run.Entrance = entrance->Loc;
    run.Roles = roles;
    run.Size = size;
    TC_LOG_INFO("misc", "ForeverBots: Group Finder run of {} to {} (map {}, roles {}, {} members)", owner->GetName(), name, run.Entrance.GetMapId(), roles, size);
    chat.SendSysMessage(Trinity::StringFormat("ForeverBots: {} ({} players) - the group is on its way.", name, size));
    return true;
}

// tanks and healers of a group of that size: 5 -> 1/1, 10 -> 2/3, 20 -> 3/5, 40 -> 4/10
uint32 ForeverBotMgr::TanksFor(uint32 size) { return size <= 5 ? 1 : size <= 10 ? 2 : size <= 20 ? 3 : 4; }
uint32 ForeverBotMgr::HealersFor(uint32 size) { return size <= 5 ? 1 : (size + 2) / 4; }

void ForeverBotMgr::UpdateDungeonRuns(uint32 diff)
{
    for (auto itr = _dungeonRuns.begin(); itr != _dungeonRuns.end();)
    {
        DungeonRun& run = itr->second;
        Player* owner = ObjectAccessor::FindConnectedPlayer(itr->first);
        run.Timer += diff;
        if (!owner || (!run.Planned && run.Timer > 2 * MINUTE * IN_MILLISECONDS))
        {
            itr = _dungeonRuns.erase(itr);
            continue;
        }
        ChatHandler chat(owner->GetSession());
        if (!run.Teleported)
        {
            if (run.Timer < DungeonRunTeleportDelay || !owner->IsInWorld() || owner->IsBeingTeleported())
            {
                ++itr;
                continue;
            }
            run.Teleported = true;

            // more than five: a raid group, and the raid maps only let a raid group in. The owner may still be alone: the
            // group then starts with them only, the bots join it inside.
            if (run.Size > 5)
            {
                Group* group = owner->GetGroup();
                if (!group)
                {
                    group = new Group();
                    if (group->Create(owner))
                        sGroupMgr->AddGroup(group);
                    else
                    {
                        delete group;
                        group = nullptr;
                    }
                }
                if (group && !group->isRaidGroup())
                    group->ConvertToRaid();
            }

            if (owner->GetMapId() != run.Entrance.GetMapId()
                && (owner->IsInCombat() || !owner->IsAlive() || !owner->TeleportTo(run.Entrance)))
            {
                chat.SendSysMessage("ForeverBots: could not take you inside.");
                // do not leave the owner alone in the group made above
                if (Group* group = owner->GetGroup(); group && group->GetMembersCount() <= 1)
                    group->Disband();
                itr = _dungeonRuns.erase(itr);
                continue;
            }
        }
        // still on the loading screen
        if (!owner->IsInWorld() || owner->IsBeingTeleported() || owner->GetMapId() != run.Entrance.GetMapId())
        {
            ++itr;
            continue;
        }

        if (!run.Planned)
        {
            run.Planned = true;

            // who is already there: the owner with the role they ticked (damage when none), their bots, other real members
            uint32 tanks = (run.Roles & lfg::PLAYER_ROLE_TANK) ? 1 : 0;
            uint32 healers = !tanks && (run.Roles & lfg::PLAYER_ROLE_HEALER) ? 1 : 0;
            std::set<uint8> classes = { owner->GetClass() };
            std::vector<ForeverBot*> bots = GetBotsOf(owner->GetGUID());
            for (ForeverBot const* bot : bots)
            {
                tanks += bot->Role == BotRole::Tank;
                healers += bot->Role == BotRole::Heal;
                if (CharacterCacheEntry const* info = sCharacterCache->GetCharacterCacheByGuid(bot->Guid))
                    classes.insert(info->Class);
            }
            size_t members = 1 + bots.size();
            if (Group const* group = owner->GetGroup())
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                    if (slot.guid != owner->GetGUID() && !IsBot(slot.guid))
                        ++members;
            // the bots read it in BotContext::RoleOf: a player who ticked tank is the group's tank, they wait for their pulls
            _ownerRoles[owner->GetGUID()] = run.Roles;

            // tanks first, then healers, then damage dealers: a group cut short by an error still has its tank and healer
            bool const horde = owner->GetTeamId() == TEAM_HORDE;
            uint8 const healerClasses[] = { CLASS_PRIEST, CLASS_PRIEST, CLASS_DRUID, uint8(horde ? CLASS_SHAMAN : CLASS_PALADIN) };
            for (uint32 i = tanks; i < TanksFor(run.Size) && members + run.Recruits.size() < run.Size; ++i)
                run.Recruits.emplace_back(uint8(CLASS_WARRIOR), BotRole::Tank);
            for (uint32 i = healers; i < HealersFor(run.Size) && members + run.Recruits.size() < run.Size; ++i)
                run.Recruits.emplace_back(healerClasses[i % std::size(healerClasses)], BotRole::Heal);
            // damage dealers: classes the group does not have yet first
            std::vector<uint8> damage = { CLASS_MAGE, CLASS_ROGUE, CLASS_HUNTER, CLASS_WARLOCK };
            std::ranges::stable_partition(damage, [&](uint8 classId) { return !classes.contains(classId); });
            for (size_t i = 0; members + run.Recruits.size() < run.Size; ++i)
                run.Recruits.emplace_back(damage[i % damage.size()], BotRole::Dps);

            TC_LOG_INFO("misc", "ForeverBots: Group Finder run of {}: {} bots to recruit ({} members before, size {})", owner->GetName(), run.Recruits.size(), members, run.Size);
            chat.SendSysMessage(Trinity::StringFormat("ForeverBots: {} bots are joining your group.", run.Recruits.size()));
        }

        // a few bots per second: every login loads a character from the database
        run.RecruitTimer += diff;
        if (!run.Recruits.empty() && run.RecruitTimer >= DungeonRunRecruitInterval)
        {
            run.RecruitTimer = 0;
            auto const [classId, role] = run.Recruits.front();
            run.Recruits.erase(run.Recruits.begin());
            std::string const error = AddBot(owner, classId, role, run.Size - 1);
            if (error.empty())
                ++run.Added;
            else
                chat.SendSysMessage(Trinity::StringFormat("ForeverBots: {}", error));
        }
        if (!run.Recruits.empty())
        {
            ++itr;
            continue;
        }

        // the group is complete: take the post down
        TC_LOG_INFO("misc", "ForeverBots: Group Finder run of {}: {} bots recruited", owner->GetName(), run.Added);
        GroupFinderListings::Delist(owner);
        itr = _dungeonRuns.erase(itr);
    }
}

void ForeverBotMgr::Update(uint32 diff)
{
    UpdateDungeonRuns(diff);

    std::vector<ObjectGuid> toRemove;

    for (auto& [guid, bot] : _bots)
    {
        if (!bot.Session)
        {
            toRemove.push_back(guid);
            continue;
        }

        // the world pass of the session: runs the save and login callbacks
        WorldSessionFilter filter(bot.Session);
        bot.Session->Update(diff, filter);

        Player* botPlayer = bot.Session->GetPlayer();

        // nobody answers the teleport packets of a bot (graveyard, spells, near teleports...): finish them here
        if (botPlayer && botPlayer->IsBeingTeleported())
            bot.Session->FinishBotTeleport();

        if (!botPlayer || !botPlayer->IsInWorld())
        {
            if (bot.CreateFailed || (!bot.Creating && !bot.Session->PlayerLoading() && !botPlayer))
            {
                TC_LOG_ERROR("misc", "ForeverBots: bot {} could not log in", guid.ToString());
                if (Player* owner = ObjectAccessor::FindConnectedPlayer(bot.Owner))
                    ChatHandler(owner->GetSession()).SendSysMessage("ForeverBots: a bot could not log in.");
                toRemove.push_back(guid);
            }
            continue;
        }

        if (!bot.Ready)
        {
            InitializeInWorld(bot, botPlayer, bot.IsWorldBot() ? nullptr : ObjectAccessor::FindConnectedPlayer(bot.Owner));
            continue;
        }

        if (bot.IsWorldBot())
            UpdateWorldBot(bot, diff, toRemove);
        else if (bot.Leader)
            UpdateLeaderBot(bot, diff);
        else
            UpdatePartyBot(bot, diff, toRemove);
    }

    for (ObjectGuid guid : toRemove)
        RemoveBot(guid);

    // a bot-only party's leader is in the world: recruit its members (outside the loop, AddBot changes _bots)
    std::vector<std::pair<ObjectGuid, std::vector<std::pair<uint8, BotRole>>>> recruits;
    for (auto& [guid, bot] : _bots)
        if (bot.Leader && bot.Ready && !bot.PendingRecruits.empty())
            recruits.emplace_back(guid, std::move(bot.PendingRecruits));
    for (auto& [leaderGuid, members] : recruits)
    {
        ForeverBot* leader = GetBot(leaderGuid);
        Player* leaderPlayer = leader && leader->Session ? leader->Session->GetPlayer() : nullptr;
        if (!leaderPlayer)
            continue;
        for (auto const& [classId, role] : members)
        {
            std::string error = AddBot(leaderPlayer, classId, role, uint32(members.size()));
            if (!error.empty())
                TC_LOG_ERROR("misc", "ForeverBots: test party member ({} {}) not added: {}", classId, BotRoleName(role), error);
        }
    }

    _balanceTimer += diff;
    if (_balanceTimer >= BalanceInterval)
    {
        _balanceTimer = 0;
        if (_worldTarget || CountWorldBots())
            BalanceWorldBots();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Bot-only parties (.bottest): a leader bot (the tank) and members owned by it, for tests without a client
// ---------------------------------------------------------------------------------------------------------------------
std::string ForeverBotMgr::StartTestParty(uint32 mapId, Position const& pos, uint8 level, uint8 team, std::vector<std::pair<uint8, BotRole>> const& members)
{
    if (members.empty())
        return "No members.";

    uint32 accountId = GetBotAccountId();
    if (!accountId)
        return "Could not create the bot account.";

    WorldSession* session = CreateBotSession(accountId, PartyAccountName);
    std::string error;
    CharacterDatabaseTransaction pendingCommit;
    ObjectGuid guid = FindOrCreateCharacter(team, members.front().first, accountId, session, error, pendingCommit);
    if (guid.IsEmpty())
    {
        delete session;
        return error;
    }

    ForeverBot& bot = _bots[guid];
    bot.Guid = guid;
    bot.Leader = true;
    bot.Role = members.front().second;
    bot.WorldLevel = level;
    bot.Session = session;
    bot.PendingRecruits.assign(members.begin() + 1, members.end());

    if (pendingCommit)
    {
        bot.Creating = true;
        session->AddTransactionCallback(CharacterDatabase.AsyncCommitTransaction(pendingCommit)).AfterComplete([this, guid, mapId, pos](bool success)
        {
            ForeverBot* created = GetBot(guid);
            if (!created)
                return;
            created->Creating = false;
            if (!success)
            {
                created->CreateFailed = true;
                return;
            }
            PlaceCharacterAt(guid, mapId, pos);
            StartWorldLogin(*created);
        });
    }
    else
    {
        PlaceCharacterAt(guid, mapId, pos);
        StartWorldLogin(bot);
    }
    TC_LOG_INFO("misc", "ForeverBots: test party starting on map {} ({} members, level {})", mapId, members.size(), level);
    return {};
}

void ForeverBotMgr::StopTestParties()
{
    std::vector<ObjectGuid> leaders;
    for (auto const& [guid, bot] : _bots)
        if (bot.Leader)
            leaders.push_back(guid);
    for (ObjectGuid leader : leaders)
    {
        RemoveBotsOf(leader);
        RemoveBot(leader);
    }
}

void ForeverBotMgr::UpdateLeaderBot(ForeverBot& bot, uint32 diff)
{
    Player* botPlayer = bot.Session->GetPlayer();
    if (!botPlayer->isDead())
    {
        bot.DeadTimer = 0;
        return;
    }

    if (!bot.DeadTimer)
        ++bot.Deaths;
    bot.DeadTimer += diff;

    // the healer gets its chance; after a wipe (nobody fighting any more) the leader stands back up
    bool fighting = false;
    for (ForeverBot* member : GetBotsOf(bot.Guid))
        if (Player* memberPlayer = member->Session ? member->Session->GetPlayer() : nullptr)
            if (memberPlayer->IsAlive() && memberPlayer->IsInCombat())
                fighting = true;
    if (!fighting && bot.DeadTimer >= ResurrectDelay)
    {
        botPlayer->ResurrectPlayer(0.5f);
        botPlayer->SpawnCorpseBones();
        bot.DeadTimer = 0;
    }
}

void ForeverBotMgr::SetDebug(bool on)
{
    _debug = on;
    for (auto& [guid, bot] : _bots)
        bot.Debug = on;
}

std::vector<std::string> ForeverBotMgr::DescribeStats()
{
    std::vector<std::string> lines;
    for (auto const& [guid, bot] : _bots)
    {
        Player* p = bot.Session ? bot.Session->GetPlayer() : nullptr;
        if (!p || !p->IsInWorld())
            continue;
        std::string items;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items += Trinity::StringFormat(" {}:{}(il{})", slot, item->GetEntry(), item->GetTemplate()->GetBaseItemLevel());
        Unit* victim = p->GetVictim();
        lines.push_back(Trinity::StringFormat("{} lvl {} class {} {} | hp {}/{} armor {} | str {} agi {} sta {} int {} spi {} | ap {} rap {} | mh {:.0f}-{:.0f} speed {} | victim {}{}",
            p->GetName(), p->GetLevel(), p->GetClass(), BotRoleName(bot.Role), p->GetHealth(), p->GetMaxHealth(), p->GetArmor(),
            p->GetStat(STAT_STRENGTH), p->GetStat(STAT_AGILITY), p->GetStat(STAT_STAMINA), p->GetStat(STAT_INTELLECT), p->GetStat(STAT_SPIRIT),
            p->GetTotalAttackPowerValue(BASE_ATTACK), p->GetTotalAttackPowerValue(RANGED_ATTACK),
            p->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE), p->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE), p->GetBaseAttackTime(BASE_ATTACK),
            victim ? Trinity::StringFormat("{} lvl {} hp {}/{} armor {} dmg {:.0f}-{:.0f}", victim->GetName(), victim->GetLevel(), victim->GetHealth(), victim->GetMaxHealth(),
                victim->GetArmor(), victim->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE), victim->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE)) : std::string("-"),
            items.empty() ? std::string(" | NO GEAR") : " | gear" + items));
    }
    return lines;
}

std::vector<std::string> ForeverBotMgr::DescribeTelemetry()
{
    std::vector<std::string> lines;
    for (auto const& [guid, bot] : _bots)
    {
        Player* p = bot.Session ? bot.Session->GetPlayer() : nullptr;
        if (!p || !p->IsInWorld())
            continue;
        float const mana = p->GetMaxPower(POWER_MANA) ? p->GetPowerPct(POWER_MANA) : 0.0f;
        float const seconds = std::max(1.0f, float(bot.CombatTime) / 1000.0f);
        lines.push_back(Trinity::StringFormat("{} lvl {} {} {}{} | {} hp {:.0f}% mana {:.0f}% | dps {:.0f} hps {:.0f} overheal {} | deaths {} kills {} interrupts {} dispels {} taunts {} cc {} res {} | casts ok {} failed {} | {}",
            p->GetName(), p->GetLevel(), BotRoleName(bot.Role), bot.Leader ? "leader " : "", bot.IsWorldBot() ? "world" : "party",
            bot.State, p->GetHealthPct(), mana, float(bot.DamageDone) / seconds, float(bot.HealingDone) / seconds, bot.Overhealing,
            bot.Deaths, bot.Kills, bot.Interrupts, bot.Dispels, bot.Taunts, bot.CrowdControls, bot.Resurrections, bot.CastsOk, bot.CastsFailed, bot.Decision));
    }
    return lines;
}
