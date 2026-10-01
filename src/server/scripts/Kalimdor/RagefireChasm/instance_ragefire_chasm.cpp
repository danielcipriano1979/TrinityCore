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

#include "ScriptMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ragefire_chasm.h"

// Forever: Classic Ragefire Chasm. The Cataclysm revamp script (Adarogg, Koranthal, Slagmaw, Gordoth with their
// DungeonEncounter ids) doesn't match the Classic 1.60 data and crashed the server on entering.
static constexpr ObjectData creatureData[] =
{
    { NPC_OGGLEFLINT, BOSS_OGGLEFLINT },
    { NPC_TARAGAMAN,  BOSS_TARAGAMAN  },
    { NPC_JERGOSH,    BOSS_JERGOSH    },
    { NPC_BAZZALAN,   BOSS_BAZZALAN   },
};

static constexpr char const* BossNames[EncounterCount] = { "Oggleflint", "Taragaman", "Jergosh", "Bazzalan" };

class instance_ragefire_chasm : public InstanceMapScript
{
public:
    instance_ragefire_chasm() : InstanceMapScript(RfCScriptName, 389) { }

    struct instance_ragefire_chasm_InstanceMapScript : public InstanceScript
    {
        instance_ragefire_chasm_InstanceMapScript(InstanceMap* map) : InstanceScript(map)
        {
            SetHeaders(DataHeader);
            SetBossNumber(EncounterCount);
            LoadObjectData(creatureData, {});
        }

        void Create() override
        {
            InstanceScript::Create();
            TC_LOG_INFO("server.ragefire", "[RFC] Instance {} created", instance->GetInstanceId());
        }

        bool SetBossState(uint32 id, EncounterState state) override
        {
            if (!InstanceScript::SetBossState(id, state))
                return false;

            if (id < EncounterCount)
            {
                switch (state)
                {
                    case IN_PROGRESS: TC_LOG_INFO("server.ragefire", "[RFC] {} engaged (instance {})", BossNames[id], instance->GetInstanceId()); break;
                    case NOT_STARTED: TC_LOG_INFO("server.ragefire", "[RFC] {} reset (instance {})", BossNames[id], instance->GetInstanceId()); break;
                    case DONE:        TC_LOG_INFO("server.ragefire", "[RFC] {} defeated (instance {})", BossNames[id], instance->GetInstanceId()); break;
                    default: break;
                }
            }

            if (state == DONE)
            {
                bool allDone = true;
                for (uint32 boss = 0; boss < EncounterCount; ++boss)
                    if (GetBossState(boss) != DONE)
                        allDone = false;
                if (allDone)
                    TC_LOG_INFO("server.ragefire", "[RFC] Instance {} completed", instance->GetInstanceId());
            }
            return true;
        }
    };

    InstanceScript* GetInstanceScript(InstanceMap* map) const override
    {
        return new instance_ragefire_chasm_InstanceMapScript(map);
    }
};

void AddSC_instance_ragefire_chasm()
{
    new instance_ragefire_chasm();
}
