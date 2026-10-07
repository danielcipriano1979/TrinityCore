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

#ifndef TRINITY_GROUP_FINDER_LISTINGS_H
#define TRINITY_GROUP_FINDER_LISTINGS_H

#include "Define.h"
#include <functional>
#include <vector>

class Group;
class Player;

// The Classic Group Finder listings (GuildHandler.cpp) write every change to the "groupfinder" log, one JSON line each, for the
// Discord bot's LFG channels (contrib/discord_bot). These keep a posted group current when its members change.
namespace GroupFinderListings
{
    TC_GAME_API void OnGroupChanged(Group const* group);
    TC_GAME_API void OnLogout(Player const* player);

    // Forever bots (scripts/Custom/ForeverBots): called when a player posts a group in the Group Finder window, with the ticked
    // activities (GroupFinderActivity ids) and the roles the player ticked (lfg::LfgRoles mask). Returns true when it takes the
    // group over (fills it with bots and takes it into the dungeon).
    using ListedHook = std::function<bool(Player* player, std::vector<uint32> const& activities, uint8 roles)>;
    TC_GAME_API void SetListedHook(ListedHook hook);
    // takes the player's listing down, as if they pressed "Delist"
    TC_GAME_API void Delist(Player* player);
}

#endif
