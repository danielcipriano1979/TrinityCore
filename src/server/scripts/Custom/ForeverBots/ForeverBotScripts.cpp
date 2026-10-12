/*
 * Forever bots: the .bot command (also sent by the ForeverBots addon through the TrinityCore addon channel)
 * and the world hooks that drive the manager.
 *
 *   .bot add <class> [tank|heal|dps]   recruit a bot of your faction and level into your group
 *   .bot remove [name|all]             dismiss one bot or all of them
 *   .bot list                          your bots with their role ("ForeverBots: list Name class role")
 *   .bot follow | stay | attack        orders to all your bots (attack = your current target)
 *   .bot passive | active              stop / resume fighting
 *   .bot summon                        teleport your bots next to you
 *   .bot world [count]                 world bots: keeps <count> of them hunting around the world (0 = none)
 *   .bot debug [off]                   your bots log every decision ([BotAI] in Server.log)
 *   .bot info                          your bots: state, decision, telemetry
 *
 * Group Finder (the game's own window, ForeverBots.GroupFinder = 1): posting a group for a dungeon takes the player into it and
 * recruits the bots the group is missing (ForeverBotMgr::StartDungeonRun).
 *
 * Console / GM: .botinfo, .botworld [n], .botdebug [on|off], .bottest party rfc [level] | stop | report
 *
 * The world bot count set by .bot world is kept across restarts (world_variable 'ForeverBots.WorldBots'); before
 * it was ever set, ForeverBots.WorldBots in worldserver.conf (default 0) is used.
 */

#include "ForeverBots.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "GroupFinderListings.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

namespace
{
    struct ClassAlias { char const* Name; uint8 ClassId; };
    ClassAlias const ClassAliases[] =
    {
        { "warrior", CLASS_WARRIOR }, { "guerreiro", CLASS_WARRIOR },
        { "paladin", CLASS_PALADIN }, { "paladino", CLASS_PALADIN },
        { "hunter", CLASS_HUNTER }, { "cacador", CLASS_HUNTER },
        { "rogue", CLASS_ROGUE }, { "ladino", CLASS_ROGUE },
        { "priest", CLASS_PRIEST }, { "sacerdote", CLASS_PRIEST },
        { "shaman", CLASS_SHAMAN }, { "xama", CLASS_SHAMAN },
        { "mage", CLASS_MAGE }, { "mago", CLASS_MAGE },
        { "warlock", CLASS_WARLOCK }, { "bruxo", CLASS_WARLOCK },
        { "druid", CLASS_DRUID }, { "druida", CLASS_DRUID },
    };

    char const* ClassName(uint8 classId)
    {
        for (ClassAlias const& alias : ClassAliases)
            if (alias.ClassId == classId)
                return alias.Name;     // the English name comes first
        return "?";
    }

    bool CanTake(uint8 classId, BotRole role)
    {
        switch (role)
        {
            case BotRole::Tank: return classId == CLASS_WARRIOR || classId == CLASS_PALADIN || classId == CLASS_DRUID;
            case BotRole::Heal: return classId == CLASS_PRIEST || classId == CLASS_PALADIN || classId == CLASS_SHAMAN || classId == CLASS_DRUID;
            default:            return true;
        }
    }

    BotRole DefaultRole(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return BotRole::Tank;
            case CLASS_PRIEST:  return BotRole::Heal;
            default:            return BotRole::Dps;
        }
    }

    // a class that is good at a role, when the addon just asks for "tank", "heal" or "dps"
    uint8 ClassForRole(BotRole role)
    {
        switch (role)
        {
            case BotRole::Tank: return CLASS_WARRIOR;
            case BotRole::Heal: return CLASS_PRIEST;
            default:
            {
                static uint8 const dps[] = { CLASS_MAGE, CLASS_HUNTER, CLASS_ROGUE, CLASS_WARLOCK };
                return dps[urand(0, std::size(dps) - 1)];
            }
        }
    }

    Optional<BotRole> ParseRole(std::string_view word)
    {
        if (word == "tank") return BotRole::Tank;
        if (word == "heal" || word == "healer") return BotRole::Heal;
        if (word == "dps" || word == "damage") return BotRole::Dps;
        return {};
    }
}

class forever_bots_commandscript : public CommandScript
{
public:
    forever_bots_commandscript() : CommandScript("forever_bots_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "bot", HandleBotCommand, rbac::RBAC_PERM_COMMAND_COMMANDS, Console::No },   // every player, like .commands
            // console / GM: state of every bot, and the world bot count (also from the RA console)
            { "botinfo", HandleBotInfoCommand, rbac::RBAC_PERM_COMMAND_SERVER_INFO, Console::Yes },
            { "botworld", HandleBotWorldCommand, rbac::RBAC_PERM_COMMAND_SERVER_INFO, Console::Yes },
            // console / GM: decision logs of every bot, telemetry, and bot-only test parties
            { "botdebug", HandleBotDebugCommand, rbac::RBAC_PERM_COMMAND_SERVER_INFO, Console::Yes },
            { "bottest", HandleBotTestCommand, rbac::RBAC_PERM_COMMAND_SERVER_INFO, Console::Yes },
        };
        return commandTable;
    }

    static bool HandleBotDebugCommand(ChatHandler* handler, Optional<std::string> state)
    {
        bool const on = !state || *state == "on" || *state == "1";
        sForeverBotMgr->SetDebug(on);
        handler->SendSysMessage(Trinity::StringFormat("ForeverBots: decision logs {} (Server.log, [BotAI]).", on ? "on" : "off"));
        return true;
    }

    // .bottest party rfc [level] | party tele <name> [level] [alliance] | stop | report
    static bool HandleBotTestCommand(ChatHandler* handler, Tail args)
    {
        std::vector<std::string_view> words = Trinity::Tokenize(args, ' ', false);
        std::string action = words.empty() ? "" : std::string(words[0]);
        strToLower(action);

        if (action == "stop")
        {
            sForeverBotMgr->StopTestParties();
            handler->SendSysMessage("ForeverBots: test parties removed.");
            return true;
        }
        if (action == "report")
        {
            for (std::string const& line : sForeverBotMgr->DescribeTelemetry())
                handler->SendSysMessage(line);
            return true;
        }
        if (action == "additem" && words.size() >= 2)
        {
            for (std::string const& line : sForeverBotMgr->TestAddItem(Trinity::StringTo<uint32>(words[1]).value_or(0)))
                handler->SendSysMessage(line);
            return true;
        }
        if (action == "stats")
        {
            for (std::string const& line : sForeverBotMgr->DescribeStats())
                handler->SendSysMessage(line);
            return true;
        }
        if (action != "party" || words.size() < 2)
        {
            handler->SendSysMessage("Usage: .bottest party rfc [level] | party tele <name> [level] [alliance] | stop | report");
            return true;
        }

        uint32 mapId = 0;
        Position pos;
        uint8 level = 15;
        uint8 team = TEAM_HORDE;
        size_t next = 2;
        std::string where(words[1]);
        strToLower(where);
        if (where == "rfc")
        {
            mapId = 389;                                            // Ragefire Chasm, at the entrance
            pos.Relocate(0.797643f, -8.23429f, -15.5288f, 4.71f);
        }
        else if (where == "tele" && words.size() >= 3)
        {
            GameTele const* tele = sObjectMgr->GetGameTele(std::string(words[2]));
            if (!tele)
            {
                handler->SendSysMessage("ForeverBots: unknown teleport location.");
                return true;
            }
            mapId = tele->mapId;
            pos.Relocate(tele->position_x, tele->position_y, tele->position_z, tele->orientation);
            next = 3;
        }
        else
        {
            handler->SendSysMessage("Usage: .bottest party rfc [level] | party tele <name> [level] [alliance] [size]");
            return true;
        }
        if (words.size() > next)
            if (Optional<uint32> value = Trinity::StringTo<uint32>(words[next]))
                level = uint8(std::clamp<uint32>(*value, 1, 60));
        // then, in any order: "alliance" and the group size (10, 20, 40: a raid group like the Group Finder builds)
        uint32 size = 5;
        for (size_t i = next + 1; i < words.size(); ++i)
        {
            if (words[i] == "alliance")
                team = TEAM_ALLIANCE;
            else if (Optional<uint32> value = Trinity::StringTo<uint32>(words[i]))
                size = std::clamp<uint32>(*value, 5, 40);
        }

        // tank (leader), healer and three damage dealers
        std::vector<std::pair<uint8, BotRole>> members =
        {
            { CLASS_WARRIOR, BotRole::Tank }, { CLASS_PRIEST, BotRole::Heal }, { CLASS_MAGE, BotRole::Dps },
            { CLASS_ROGUE, BotRole::Dps }, { team == TEAM_HORDE ? CLASS_WARLOCK : CLASS_HUNTER, BotRole::Dps }
        };
        if (size > 5)
        {
            members.clear();
            uint8 const healers[] = { CLASS_PRIEST, CLASS_PRIEST, CLASS_DRUID, uint8(team == TEAM_HORDE ? CLASS_SHAMAN : CLASS_PALADIN) };
            uint8 const damage[] = { CLASS_MAGE, CLASS_ROGUE, CLASS_HUNTER, CLASS_WARLOCK };
            for (uint32 i = 0; i < ForeverBotMgr::TanksFor(size); ++i)
                members.emplace_back(uint8(CLASS_WARRIOR), BotRole::Tank);
            for (uint32 i = 0; i < ForeverBotMgr::HealersFor(size); ++i)
                members.emplace_back(healers[i % std::size(healers)], BotRole::Heal);
            for (uint32 i = 0; members.size() < size; ++i)
                members.emplace_back(damage[i % std::size(damage)], BotRole::Dps);
        }
        std::string error = sForeverBotMgr->StartTestParty(mapId, pos, level, team, members);
        handler->SendSysMessage(error.empty() ? Trinity::StringFormat("ForeverBots: test party (level {}) logging in on map {}.", level, mapId)
            : Trinity::StringFormat("ForeverBots: {}", error));
        return true;
    }

    static bool HandleBotWorldCommand(ChatHandler* handler, Optional<uint32> count)
    {
        if (count)
            sForeverBotMgr->SetWorldBotCount(*count);
        handler->SendSysMessage(Trinity::StringFormat("ForeverBots: world bots {} online, target {}.", sForeverBotMgr->CountWorldBots(), sForeverBotMgr->GetWorldBotTarget()));
        return true;
    }

    static bool HandleBotInfoCommand(ChatHandler* handler)
    {
        for (std::string const& line : sForeverBotMgr->DescribeBots())
            handler->SendSysMessage(line);
        return true;
    }

    static bool HandleBotCommand(ChatHandler* handler, Tail args)
    {
        Player* player = handler->GetSession()->GetPlayer();
        // answers go to the player's chat, also when the command came through the addon channel
        ChatHandler chat(handler->GetSession());

        if (sForeverBotMgr->IsBot(player->GetGUID()))
            return true;

        std::vector<std::string_view> words = Trinity::Tokenize(args, ' ', false);
        std::string action = words.empty() ? "" : std::string(words[0]);
        strToLower(action);

        if (action == "add")
        {
            if (words.size() < 2)
            {
                chat.SendSysMessage("Usage: .bot add <class|tank|heal|dps> [tank|heal|dps]");
                return true;
            }

            std::string first(words[1]);
            strToLower(first);
            uint8 classId = 0;
            Optional<BotRole> role = ParseRole(first);
            if (role)
                classId = ClassForRole(*role);
            else
            {
                for (ClassAlias const& alias : ClassAliases)
                    if (first == alias.Name)
                        classId = alias.ClassId;
                if (!classId)
                {
                    chat.SendSysMessage(Trinity::StringFormat("ForeverBots: unknown class '{}'.", first));
                    return true;
                }
                if (words.size() >= 3)
                {
                    std::string second(words[2]);
                    strToLower(second);
                    role = ParseRole(second);
                }
                if (!role)
                    role = DefaultRole(classId);
            }

            if (!CanTake(classId, *role))
            {
                chat.SendSysMessage(Trinity::StringFormat("ForeverBots: a {} cannot be a {}.", ClassName(classId), BotRoleName(*role)));
                return true;
            }

            std::string error = sForeverBotMgr->AddBot(player, classId, *role);
            if (!error.empty())
                chat.SendSysMessage(Trinity::StringFormat("ForeverBots: {}", error));
            else
                chat.SendSysMessage(Trinity::StringFormat("ForeverBots: a {} {} is on the way.", ClassName(classId), BotRoleName(*role)));
            return true;
        }

        if (action == "world")
        {
            if (words.size() >= 2)
            {
                Optional<uint32> count = Trinity::StringTo<uint32>(words[1]);
                if (!count)
                {
                    chat.SendSysMessage("Usage: .bot world <count> (0 logs every world bot out)");
                    return true;
                }
                sForeverBotMgr->SetWorldBotCount(*count);
            }
            chat.SendSysMessage(Trinity::StringFormat("ForeverBots: world bots {} online, target {}.", sForeverBotMgr->CountWorldBots(), sForeverBotMgr->GetWorldBotTarget()));
            return true;
        }

        std::vector<ForeverBot*> bots = sForeverBotMgr->GetBotsOf(player->GetGUID());

        if (action == "list")
        {
            if (bots.empty())
                chat.SendSysMessage("ForeverBots: you have no bots. Use .bot add <class>.");
            for (ForeverBot* bot : bots)
            {
                Player* botPlayer = bot->Session ? bot->Session->GetPlayer() : nullptr;
                chat.SendSysMessage(Trinity::StringFormat("ForeverBots: list {} {} {}", botPlayer ? botPlayer->GetName() : "(loading)",
                    botPlayer ? ClassName(botPlayer->GetClass()) : "?", BotRoleName(bot->Role)));
            }
            return true;
        }

        if (bots.empty())
        {
            chat.SendSysMessage("ForeverBots: you have no bots. Use .bot add <class>.");
            return true;
        }

        if (action == "remove")
        {
            std::string name = words.size() >= 2 ? std::string(words[1]) : "all";
            strToLower(name);
            for (ForeverBot* bot : bots)
            {
                Player* botPlayer = bot->Session ? bot->Session->GetPlayer() : nullptr;
                std::string botName = botPlayer ? botPlayer->GetName() : "";
                strToLower(botName);
                if (name == "all" || name == botName)
                    sForeverBotMgr->RemoveBot(bot->Guid);
            }
            chat.SendSysMessage("ForeverBots: dismissed.");
            return true;
        }

        if (action == "debug")
        {
            bool const on = words.size() < 2 || words[1] != "off";
            for (ForeverBot* bot : bots)
                bot->Debug = on;
            chat.SendSysMessage(Trinity::StringFormat("ForeverBots: decision logs {} for your bots (Server.log, [BotAI]).", on ? "on" : "off"));
            return true;
        }

        if (action == "info")
        {
            for (std::string const& line : sForeverBotMgr->DescribeTelemetry())
                for (ForeverBot* bot : bots)
                    if (Player* botPlayer = bot->Session ? bot->Session->GetPlayer() : nullptr; botPlayer && line.starts_with(botPlayer->GetName()))
                        chat.SendSysMessage(line);
            return true;
        }

        for (ForeverBot* bot : bots)
        {
            if (action == "follow")       { bot->Order = BotOrder::Follow; bot->Passive = false; }
            else if (action == "stay")    { bot->Order = BotOrder::Stay; bot->Passive = false; }
            else if (action == "attack")  { bot->Order = BotOrder::Attack; bot->Passive = false; }
            else if (action == "passive") { bot->Passive = true; }
            else if (action == "active")  { bot->Passive = false; }
            else if (action == "summon")  { bot->SummonRequested = true; }
            else
            {
                chat.SendSysMessage("Usage: .bot add <class> [tank|heal|dps] | remove [name|all] | list | follow | stay | attack | passive | active | summon | world [count]");
                return true;
            }
        }
        chat.SendSysMessage(Trinity::StringFormat("ForeverBots: order {}.", action));
        return true;
    }
};

class forever_bots_worldscript : public WorldScript
{
public:
    forever_bots_worldscript() : WorldScript("forever_bots_worldscript") { }

    void OnStartup() override
    {
        sForeverBotMgr->LoadClassicSpells();
        sForeverBotMgr->SetDebug(sConfigMgr->GetBoolDefault("ForeverBots.Debug", false));
        sForeverBotMgr->LoadWorldBotCount(uint32(sConfigMgr->GetIntDefault("ForeverBots.WorldBots", 0)));
        if (sConfigMgr->GetBoolDefault("ForeverBots.GroupFinder", true))
            GroupFinderListings::SetListedHook([](Player* player, uint32 category, std::vector<uint32> const& activities, uint8 roles)
            {
                return sForeverBotMgr->StartDungeonRun(player, category, activities, roles);
            });
    }

    void OnUpdate(uint32 diff) override
    {
        sForeverBotMgr->Update(diff);
    }

    void OnShutdown() override
    {
        sForeverBotMgr->RemoveAll();
    }
};

class forever_bots_playerscript : public PlayerScript
{
public:
    forever_bots_playerscript() : PlayerScript("forever_bots_playerscript") { }

    void OnCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        if (ForeverBot* bot = sForeverBotMgr->GetBot(killer->GetGUID()))
            ++bot->Kills;
    }
};

void AddSC_forever_bots()
{
    new forever_bots_commandscript();
    new forever_bots_worldscript();
    new forever_bots_playerscript();
}
