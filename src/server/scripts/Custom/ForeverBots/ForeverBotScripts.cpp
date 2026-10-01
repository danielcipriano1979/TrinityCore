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
 *
 * The world bot count starts from ForeverBots.WorldBots in worldserver.conf (default 0).
 */

#include "ForeverBots.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Language.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
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
        };
        return commandTable;
    }

    static bool HandleBotWorldCommand(ChatHandler* handler, Optional<uint32> count)
    {
        if (count)
            sForeverBotMgr->SetWorldBotCount(*count);
        handler->PSendSysMessage("ForeverBots: world bots {} online, target {}.", sForeverBotMgr->CountWorldBots(), sForeverBotMgr->GetWorldBotTarget());
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
                    chat.PSendSysMessage("ForeverBots: unknown class '{}'.", first);
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
                chat.PSendSysMessage("ForeverBots: a {} cannot be a {}.", ClassName(classId), BotRoleName(*role));
                return true;
            }

            std::string error = sForeverBotMgr->AddBot(player, classId, *role);
            if (!error.empty())
                chat.PSendSysMessage("ForeverBots: {}", error);
            else
                chat.PSendSysMessage("ForeverBots: a {} {} is on the way.", ClassName(classId), BotRoleName(*role));
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
            chat.PSendSysMessage("ForeverBots: world bots {} online, target {}.", sForeverBotMgr->CountWorldBots(), sForeverBotMgr->GetWorldBotTarget());
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
                chat.PSendSysMessage("ForeverBots: list {} {} {}", botPlayer ? botPlayer->GetName() : "(loading)",
                    botPlayer ? ClassName(botPlayer->GetClass()) : "?", BotRoleName(bot->Role));
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
        chat.PSendSysMessage("ForeverBots: order {}.", action);
        return true;
    }
};

class forever_bots_worldscript : public WorldScript
{
public:
    forever_bots_worldscript() : WorldScript("forever_bots_worldscript") { }

    void OnStartup() override
    {
        sForeverBotMgr->SetWorldBotCount(uint32(sConfigMgr->GetIntDefault("ForeverBots.WorldBots", 0)));
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

void AddSC_forever_bots()
{
    new forever_bots_commandscript();
    new forever_bots_worldscript();
}
