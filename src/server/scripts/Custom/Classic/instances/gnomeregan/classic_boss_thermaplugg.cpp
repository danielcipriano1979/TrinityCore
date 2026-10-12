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

// Classic 1.60 port of VMaNGOS src/scripts/eastern_kingdoms/dun_morogh/gnomeregan/boss_thermaplugg.cpp (ScriptDev2 lineage, GPL-2)
// Ported: boss_thermaplugg, go_gnomeface_button

#include "ScriptMgr.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "classic_gnomeregan.h"
#include "classic_script_text.h"
#include <list>

enum Thermaplugg
{
    SAY_AGGRO                           = 6173,
    SAY_PHASE                           = 6174,
    SAY_BOMB                            = 6176,
    SAY_SLAY                            = 6175,

    SPELL_ACTIVATE_BOMB_A               = 11511,            // Target Dest = -530.754 670.571 -313.784
    SPELL_ACTIVATE_BOMB_B               = 11795,            // Target Dest = -530.754 670.571 -313.784
    SPELL_KNOCK_AWAY                    = 10101,
    SPELL_KNOCK_AWAY_AOE                = 11130,
    SPELL_WALKING_BOMB_EFFECT           = 11504,

    NPC_WALKING_BOMB                    = 7915,

    POINT_BOMB_FALL                     = 1
};

namespace
{
float const GnomereganBombSpawnZ = -316.2625f;
}

struct classic_boss_thermaplugg : public ScriptedAI
{
    classic_boss_thermaplugg(Creature* creature) : ScriptedAI(creature)
    {
        _instance = dynamic_cast<classic_instance_gnomeregan_InstanceScript*>(creature->GetInstanceScript());
        Initialize();
    }

    void Initialize()
    {
        _knockAwayTimer = urand(17000, 20000);
        _activateBombTimer = urand(10000, 15000);
        _isPhaseTwo = false;
        _bombFaces = nullptr;

        _spawnPos[0] = _spawnPos[1] = _spawnPos[2] = 0.0f;
        _landedBombGUIDs.clear();
        _fallingBombGUIDs.clear();
    }

    void Reset() override
    {
        Initialize();
    }

    void KilledUnit(Unit* /*victim*/) override
    {
        ClassicScriptText(SAY_SLAY, me);
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (_instance)
            _instance->SetData(TYPE_THERMAPLUGG, DONE);

        _summonedBombGUIDs.clear();
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        ClassicScriptText(SAY_AGGRO, me);

        if (_instance)
        {
            _instance->SetData(TYPE_THERMAPLUGG, IN_PROGRESS);
            _bombFaces = _instance->GetBombFaces();
        }

        _spawnPos[0] = me->GetPositionX();
        _spawnPos[1] = me->GetPositionY();
        _spawnPos[2] = me->GetPositionZ();
    }

    void JustReachedHome() override
    {
        if (_instance)
            _instance->SetData(TYPE_THERMAPLUGG, FAIL);

        // Remove remaining bombs (a copy: DespawnOrUnsummon calls SummonedCreatureDespawn, which removes the guid from the
        // list being walked; that crashed the server on a wipe, 2026-10-11 21:43)
        std::list<ObjectGuid> const bombs = _summonedBombGUIDs;
        for (ObjectGuid const& guid : bombs)
        {
            if (Creature* bomb = me->GetMap()->GetCreature(guid))
                bomb->DespawnOrUnsummon();
        }
        _summonedBombGUIDs.clear();
    }

    void JustSummoned(Creature* summoned) override
    {
        if (summoned->GetEntry() == NPC_WALKING_BOMB)
        {
            _summonedBombGUIDs.push_back(summoned->GetGUID());
            // calculate point for falling down
            float x = 0.2f * _spawnPos[0] + 0.8f * summoned->GetPositionX();
            float y = 0.2f * _spawnPos[1] + 0.8f * summoned->GetPositionY();
            float z = _spawnPos[2] - 2.0f;
            summoned->UpdateGroundPositionZ(x, y, z);
            // VMaNGOS MovePoint(..., MOVE_FALLING)
            summoned->GetMotionMaster()->MovePoint(POINT_BOMB_FALL, x, y, z, false);
            _fallingBombGUIDs.push_back(summoned->GetGUID());
        }
    }

    void SummonedCreatureDespawn(Creature* summoned) override
    {
        _summonedBombGUIDs.remove(summoned->GetGUID());
        _fallingBombGUIDs.remove(summoned->GetGUID());
    }

    // VMaNGOS EffectDummyCreature_spell_boss_thermaplugg: "Activate Bomb" selects a random Bomb-Face and activates it if needed.
    // The spells only have a dest target in the client data, so the effect is handled when the cast completes.
    void OnSpellCast(SpellInfo const* spellInfo) override
    {
        if (spellInfo->Id != SPELL_ACTIVATE_BOMB_A && spellInfo->Id != SPELL_ACTIVATE_BOMB_B)
            return;

        if (_instance)
            _instance->DoActivateBombFace(urand(0, MAX_GNOME_FACES - 1));
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        // VMaNGOS SummonedMovementInform (not in TC): a falling bomb whose point movement finished has landed
        for (auto itr = _fallingBombGUIDs.begin(); itr != _fallingBombGUIDs.end();)
        {
            Creature* bomb = me->GetMap()->GetCreature(*itr);
            if (!bomb)
            {
                itr = _fallingBombGUIDs.erase(itr);
                continue;
            }

            if (bomb->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                _landedBombGUIDs.push_back(*itr);
                itr = _fallingBombGUIDs.erase(itr);
                continue;
            }
            ++itr;
        }

        // Movement of Summoned mobs
        if (!_landedBombGUIDs.empty())
        {
            for (ObjectGuid const& guid : _landedBombGUIDs)
            {
                if (Creature* bomb = me->GetMap()->GetCreature(guid))
                    bomb->GetMotionMaster()->MoveFollow(me, 0.0f, 0.0f);
            }
            _landedBombGUIDs.clear();
        }

        if (!_isPhaseTwo && me->GetHealthPct() < 50.0f)
        {
            ClassicScriptText(SAY_PHASE, me);
            _isPhaseTwo = true;
        }

        if (_knockAwayTimer < diff)
        {
            if (_isPhaseTwo)
            {
                if (DoCastSelf(SPELL_KNOCK_AWAY_AOE) == SPELL_CAST_OK)
                    _knockAwayTimer = 12000;
            }
            else
            {
                if (DoCastVictim(SPELL_KNOCK_AWAY) == SPELL_CAST_OK)
                    _knockAwayTimer = urand(17000, 20000);
            }
        }
        else
            _knockAwayTimer -= diff;

        if (_activateBombTimer < diff)
        {
            if (DoCastSelf(_isPhaseTwo ? SPELL_ACTIVATE_BOMB_B : SPELL_ACTIVATE_BOMB_A) == SPELL_CAST_OK)
            {
                _activateBombTimer = (_isPhaseTwo ? urand(6, 12) : urand(12, 17)) * IN_MILLISECONDS;
                if (!urand(0, 5))                           // TODO, chance/ place for this correct?
                    ClassicScriptText(SAY_BOMB, me);
            }
        }
        else
            _activateBombTimer -= diff;

        // Spawn bombs
        if (_bombFaces)
        {
            for (uint8 i = 0; i < MAX_GNOME_FACES; i++)
            {
                if (_bombFaces[i].Activated)
                {
                    if (_bombFaces[i].BombTimer < diff)
                    {
                        std::list<Creature*> bombList;
                        me->GetCreatureListWithEntryInGrid(bombList, NPC_WALKING_BOMB, 250.0f);
                        if (bombList.size() < MAX_GNOME_FACES)
                        {
                            // Calculate the spawning position as 90% between face and thermaplugg spawn-pos, and hight hardcoded
                            float x = 0.0f, y = 0.0f;
                            if (GameObject* face = me->GetMap()->GetGameObject(_bombFaces[i].GnomeFaceGUID))
                            {
                                x = 0.35f * _spawnPos[0] + 0.65f * face->GetPositionX();
                                y = 0.35f * _spawnPos[1] + 0.65f * face->GetPositionY();
                            }
                            me->SummonCreature(NPC_WALKING_BOMB, x, y, GnomereganBombSpawnZ, 0.0f, TEMPSUMMON_CORPSE_DESPAWN);
                        }
                        _bombFaces[i].BombTimer = urand(10000, 25000);   // TODO
                    }
                    else
                        _bombFaces[i].BombTimer -= diff;
                }
            }
        }
    }

private:
    classic_instance_gnomeregan_InstanceScript* _instance;
    bool _isPhaseTwo;

    uint32 _knockAwayTimer;
    uint32 _activateBombTimer;

    ClassicGnomereganBombFace* _bombFaces;
    float _spawnPos[3];

    std::list<ObjectGuid> _summonedBombGUIDs;
    std::list<ObjectGuid> _landedBombGUIDs;
    std::list<ObjectGuid> _fallingBombGUIDs;
};

/*######
## go_gnomeface_button
######*/

struct classic_go_gnomeface_button : public GameObjectAI
{
    classic_go_gnomeface_button(GameObject* go) : GameObjectAI(go) { }

    bool OnGossipHello(Player* player) override
    {
        classic_instance_gnomeregan_InstanceScript* instance = dynamic_cast<classic_instance_gnomeregan_InstanceScript*>(player->GetInstanceScript());
        if (!instance)
            return false;

        // If a button is used, the related face should be deactivated (if already activated)
        switch (me->GetEntry())
        {
            case GO_BUTTON_1:
                instance->DoDeactivateBombFace(0);
                break;
            case GO_BUTTON_2:
                instance->DoDeactivateBombFace(1);
                break;
            case GO_BUTTON_3:
                instance->DoDeactivateBombFace(2);
                break;
            case GO_BUTTON_4:
                instance->DoDeactivateBombFace(3);
                break;
            case GO_BUTTON_5:
                instance->DoDeactivateBombFace(4);
                break;
            case GO_BUTTON_6:
                instance->DoDeactivateBombFace(5);
                break;
            default:
                break;
        }

        return false;
    }
};

void AddSC_classic_boss_thermaplugg()
{
    RegisterCreatureAI(classic_boss_thermaplugg);
    RegisterGameObjectAI(classic_go_gnomeface_button);
}
