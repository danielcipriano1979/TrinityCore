/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_ITEM_ACQUIRE_HOOK_H
#define TRINITY_ITEM_ACQUIRE_HOOK_H

#include "Define.h"
#include <functional>

class Item;
class Player;
enum class ItemContext : uint8;

// Forever (scripts/Custom/ForeverRandomEnchants.cpp): called by Player::StoreNewItem for every item a player receives as a
// new item - loot, roll, quest reward, crafted, vendor (context ItemContext::Vendor), GM command. Not for mail or trades.
namespace ItemAcquireHook
{
    using Hook = std::function<void(Player* player, Item* item, ItemContext context)>;
    TC_GAME_API void Set(Hook hook);
    void Call(Player* player, Item* item, ItemContext context);
}

#endif
