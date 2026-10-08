/*
 * Forever: random enchantments on newly acquired weapons and armor.
 *
 * A port of the AzerothCore module mod-random-enchants (https://github.com/azerothcore/mod-random-enchants, 3ndos and
 * the AzerothCore contributors): same rolls, same tiers, same table (world.item_enchantment_random_tiers, reduced to the
 * enchantments the 1.60 client has, custom SQL 2026_10_08_random_enchants.sql).
 *
 * An item a player receives as loot, on a roll, as a quest reward or by crafting gets a first enchantment with
 * RandomEnchants.EnchantChance1 %, then a second with EnchantChance2 % and a third with EnchantChance3 %. The tier of each
 * one is rolled from the item's quality (poor and common mostly tier 1, legendary always tier 5). They go, like in the
 * module, to the permanent, the temporary and the socket bonus enchantment slots, so an enchanting profession enchant or a
 * weapon oil replaces the random one of its slot.
 *
 * worldserver.conf (all optional): RandomEnchants.Enable (1), RandomEnchants.OnLoot (1, loot, rolls, crafted and every
 * other new item), RandomEnchants.OnQuestReward (1), RandomEnchants.OnVendor (0), RandomEnchants.EnchantChance1/2/3
 * (70 / 65 / 60), RandomEnchants.AnnounceOnLogin (0), RandomEnchants.OnLoginMessage.
 */

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Item.h"
#include "ItemAcquireHook.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <array>
#include <vector>

namespace
{
    struct RandomEnchant
    {
        uint32 Id = 0;
        uint8 ItemClass = 0;                // ITEM_CLASS_WEAPON, ITEM_CLASS_ARMOR, or MAX_ITEM_CLASS for any
        int32 SubClass = -1;                // only for that item subclass, -1 = any
    };

    constexpr uint8 MaxTier = 5;
    std::array<std::vector<RandomEnchant>, MaxTier + 1> Tiers;

    bool Enabled = true, OnLoot = true, OnQuestReward = true, OnVendor = false, AnnounceOnLogin = false;
    float Chance[3] = { 70.0f, 65.0f, 60.0f };
    std::string LoginMessage;

    void LoadConfig()
    {
        Enabled = sConfigMgr->GetBoolDefault("RandomEnchants.Enable", true);
        OnLoot = sConfigMgr->GetBoolDefault("RandomEnchants.OnLoot", true);
        OnQuestReward = sConfigMgr->GetBoolDefault("RandomEnchants.OnQuestReward", true);
        OnVendor = sConfigMgr->GetBoolDefault("RandomEnchants.OnVendor", false);
        AnnounceOnLogin = sConfigMgr->GetBoolDefault("RandomEnchants.AnnounceOnLogin", false);
        LoginMessage = sConfigMgr->GetStringDefault("RandomEnchants.OnLoginMessage", "This server is running a RandomEnchants Module.");
        Chance[0] = sConfigMgr->GetFloatDefault("RandomEnchants.EnchantChance1", 70.0f);
        Chance[1] = sConfigMgr->GetFloatDefault("RandomEnchants.EnchantChance2", 65.0f);
        Chance[2] = sConfigMgr->GetFloatDefault("RandomEnchants.EnchantChance3", 60.0f);
    }

    void LoadTiers()
    {
        for (std::vector<RandomEnchant>& tier : Tiers)
            tier.clear();

        QueryResult result = WorldDatabase.Query("SELECT enchantID, tier, class, exclusiveSubClass FROM item_enchantment_random_tiers");
        if (!result)
        {
            TC_LOG_ERROR("server.loading", "ForeverRandomEnchants: table item_enchantment_random_tiers is empty or missing, no random enchantments (custom SQL 2026_10_08_random_enchants.sql)");
            return;
        }

        uint32 count = 0, unknown = 0;
        do
        {
            Field* fields = result->Fetch();
            RandomEnchant enchant;
            enchant.Id = fields[0].GetUInt32();
            uint8 const tier = fields[1].GetUInt8();
            std::string const itemClass = fields[2].GetString();
            enchant.ItemClass = itemClass == "WEAPON" ? uint8(ITEM_CLASS_WEAPON) : itemClass == "ARMOR" ? uint8(ITEM_CLASS_ARMOR) : uint8(MAX_ITEM_CLASS);
            enchant.SubClass = fields[3].IsNull() ? -1 : fields[3].GetInt32();
            if (tier < 1 || tier > MaxTier || !sSpellItemEnchantmentStore.LookupEntry(enchant.Id))
            {
                ++unknown;
                continue;
            }
            Tiers[tier].push_back(enchant);
            ++count;
        } while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> ForeverRandomEnchants: {} random enchantments loaded ({} skipped: unknown tier or enchantment)", count, unknown);
    }

    // the module's rarity roll: the item's quality decides which tiers are possible
    uint8 RollTier(uint32 quality)
    {
        int32 roll = -1;
        switch (quality)
        {
            case ITEM_QUALITY_POOR:      roll = int32(rand_norm() * 25); break;
            case ITEM_QUALITY_NORMAL:    roll = int32(rand_norm() * 50); break;
            case ITEM_QUALITY_UNCOMMON:  roll = 45 + int32(rand_norm() * 20); break;
            case ITEM_QUALITY_RARE:      roll = 65 + int32(rand_norm() * 15); break;
            case ITEM_QUALITY_EPIC:      roll = 80 + int32(rand_norm() * 14); break;
            case ITEM_QUALITY_LEGENDARY: roll = 93; break;
            default:                     return 0;
        }
        if (roll <= 44) return 1;
        if (roll <= 64) return 2;
        if (roll <= 79) return 3;
        if (roll <= 92) return 4;
        return 5;
    }

    uint32 RollEnchant(ItemTemplate const* proto)
    {
        uint8 const tier = RollTier(proto->GetQuality());
        if (!tier)
            return 0;

        std::vector<uint32> pool;
        for (RandomEnchant const& enchant : Tiers[tier])
            if ((enchant.ItemClass == MAX_ITEM_CLASS || enchant.ItemClass == proto->GetClass())
                && (enchant.SubClass < 0 || uint32(enchant.SubClass) == proto->GetSubClass()))
                pool.push_back(enchant.Id);
        return pool.empty() ? 0 : pool[urand(0, uint32(pool.size()) - 1)];
    }

    void RollPossibleEnchants(Player* player, Item* item, ItemContext context)
    {
        if (!Enabled || !item)
            return;
        if (context == ItemContext::Vendor ? !OnVendor : context == ItemContext::Quest_Reward ? !OnQuestReward : !OnLoot)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->GetClass() != ITEM_CLASS_WEAPON && proto->GetClass() != ITEM_CLASS_ARMOR)
            return;
        if (proto->GetQuality() > ITEM_QUALITY_LEGENDARY || proto->GetMaxStackSize() > 1)
            return;

        // like the module: the second roll only happens when the first one gave something, the third after the second
        static constexpr EnchantmentSlot Slots[3] = { PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT, BONUS_ENCHANTMENT_SLOT };
        uint32 applied = 0;
        for (uint8 i = 0; i < 3; ++i)
        {
            if (item->GetEnchantmentId(Slots[i]) || !roll_chance(Chance[i]))
                break;
            uint32 const enchantId = RollEnchant(proto);
            if (!enchantId)
                break;

            bool const equipped = item->IsEquipped();
            if (equipped)
                player->ApplyEnchantment(item, Slots[i], false);
            item->SetEnchantment(Slots[i], enchantId, 0, 0);
            if (equipped)
                player->ApplyEnchantment(item, Slots[i], true);
            ++applied;
        }
        if (!applied)
            return;

        item->SetState(ITEM_CHANGED, player);
        char const* name = proto->GetName(player->GetSession()->GetSessionDbcLocale());
        ChatHandler(player->GetSession()).SendSysMessage(Trinity::StringFormat(
            "Newly Acquired |cffFF0000 {} |rhas received|cffFF0000 {} |rrandom enchantment{}!", name ? name : "item", applied, applied > 1 ? "s" : ""));
    }
}

class forever_random_enchants_worldscript : public WorldScript
{
public:
    forever_random_enchants_worldscript() : WorldScript("forever_random_enchants_worldscript") { }

    void OnStartup() override
    {
        LoadConfig();
        LoadTiers();
        ItemAcquireHook::Set(&RollPossibleEnchants);
    }

    void OnConfigLoad(bool reload) override
    {
        if (reload)
            LoadConfig();
    }
};

class forever_random_enchants_playerscript : public PlayerScript
{
public:
    forever_random_enchants_playerscript() : PlayerScript("forever_random_enchants_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (Enabled && AnnounceOnLogin && !LoginMessage.empty())
            ChatHandler(player->GetSession()).SendSysMessage(LoginMessage);
    }
};

void AddSC_forever_random_enchants()
{
    new forever_random_enchants_worldscript();
    new forever_random_enchants_playerscript();
}
