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

// Classic 1.60 port of VMaNGOS src/scripts/eastern_kingdoms/dun_morogh/gnomeregan/gnomeregan.cpp (ScriptDev2 lineage, GPL-2)
// Grubbis encounter, quest 2904 (A Fine Mess)
// Ported: npc_blastmaster_emi_shortfuse, npc_kernobee, spell_gnomeregan_collecting_fallout

#include "ScriptMgr.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptedCreature.h"
#include "ScriptedEscortAI.h"
#include "ScriptedFollowerAI.h"
#include "ScriptedGossip.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "classic_gnomeregan.h"
#include "classic_script_text.h"
#include <list>

/*######
## npc_blastmaster_emi_shortfuse
######*/

enum BlastmasterEmiShortfuse
{
    SAY_START                   = 4050,
    SAY_INTRO_1                 = 4051,
    SAY_INTRO_2                 = 4052,
    SAY_INTRO_3                 = 4129,
    SAY_INTRO_4                 = 4130,
    SAY_LOOK_1                  = 4131,
    SAY_HEAR_1                  = 4132,
    SAY_AGGRO_1                 = 5161,
    SAY_CHARGE_1                = 4133,
    SAY_CHARGE_2                = 4134,
    SAY_BLOW_1_10               = 4135,
    SAY_BLOW_1_5                = 4136,
    SAY_BLOW_1                  = 4137,
    SAY_FINISH_1                = 4206,
    SAY_LOOK_2                  = 4207,
    SAY_HEAR_2                  = 4208,
    SAY_CHARGE_3                = 4209,
    SAY_CHARGE_4                = 4325,
    SAY_BLOW_2_10               = 4326,
    SAY_BLOW_2_5                = 4327,
    SAY_BLOW_SOON               = 4329,
    SAY_BLOW_2                  = 4137,
    SAY_FINISH_2                = 4446,

    SAY_AGGRO_2                 = 5164,

    SAY_GRUBBIS_SPAWN           = 4328,

    GOSSIP_ITEM_START           = 4084,     // broadcast text "I am ready to begin."

    SPELL_EXPLOSION_NORTH       = 12158,
    SPELL_EXPLOSION_SOUTH       = 12159,
    SPELL_FIREWORKS_RED         = 11542,

    //GO_EXPLOSIVE_CHARGE         = 144065, //A USE

    MAX_SUMMON_POSITIONS        = 33,

    NPC_GRUBBIS                 = 7361,
    NPC_CHOMPER                 = 6215,
    NPC_CAVERNDEEP_BURROWER     = 6206,
    NPC_CAVERNDEEP_AMBUSHER     = 6207
};

namespace
{
struct ClassicGnomereganSummonInformation
{
    uint32 Position, Entry;
    float X, Y, Z, O;
};

ClassicGnomereganSummonInformation const GnomereganSummonInfo[MAX_SUMMON_POSITIONS] =
{
    // Entries must be sorted by pack
    // First Cave-In
    {1, NPC_CAVERNDEEP_AMBUSHER, -566.8114f, -111.7036f, -151.1891f, 5.986479f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -568.5875f, -113.7559f, -151.1869f, 0.06981317f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -570.2333f, -116.8126f, -151.2272f, 0.296706f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -550.6331f, -108.7592f, -153.965f, 0.8901179f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -558.9717f, -115.0669f, -151.8799f, 0.5235988f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -556.6719f, -112.0526f, -152.8255f, 0.4886922f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -552.6419f, -113.4385f, -153.0727f, 0.8028514f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -549.1248f, -112.1469f, -153.7987f, 0.7504916f},
    {1, NPC_CAVERNDEEP_AMBUSHER, -546.7435f, -112.3051f, -154.2225f, 0.9250245f},
    {2, NPC_CAVERNDEEP_AMBUSHER, -571.4071f, -108.7721f, -150.6547f, 5.480334f},
    {2, NPC_CAVERNDEEP_AMBUSHER, -573.797f, -106.5265f, -150.4106f, 5.550147f},
    {2, NPC_CAVERNDEEP_AMBUSHER, -576.3784f, -108.0483f, -150.4227f, 5.585053f},
    {2, NPC_CAVERNDEEP_AMBUSHER, -576.697f, -111.7413f, -150.6484f, 5.759586f},
    {3, NPC_CAVERNDEEP_AMBUSHER, -571.3161f, -114.4412f, -151.0931f, 6.021386f},
    {3, NPC_CAVERNDEEP_AMBUSHER, -570.3127f, -111.7964f, -151.04f, 2.042035f},

    // Second Cave-In
    {4, NPC_CAVERNDEEP_AMBUSHER, -474.5954f, -104.074f, -146.0483f, 2.338741f},
    {4, NPC_CAVERNDEEP_AMBUSHER, -477.9396f, -108.6563f, -145.7394f, 1.553343f},
    {4, NPC_CAVERNDEEP_AMBUSHER, -475.6625f, -97.12168f, -146.5959f, 1.291544f},
    {4, NPC_CAVERNDEEP_AMBUSHER, -480.5233f, -88.40702f, -146.3772f, 3.001966f},
    {5, NPC_CAVERNDEEP_AMBUSHER, -474.2943f, -105.2212f, -145.9747f, 2.251475f},
    {5, NPC_CAVERNDEEP_AMBUSHER, -481.1831f, -101.4225f, -146.377f, 2.146755f},
    {5, NPC_CAVERNDEEP_BURROWER, -475.0871f, -100.016f, -146.4382f, 2.303835f},
    {5, NPC_CAVERNDEEP_AMBUSHER, -478.8562f, -106.9321f, -145.8533f, 1.658063f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -473.8762f, -107.4022f, -145.838f, 2.024582f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -490.5134f, -92.72843f, -148.0954f, 3.054326f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -491.401f, -88.25341f, -148.0358f, 3.560472f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -479.1431f, -106.227f, -145.9097f, 1.727876f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -475.3185f, -101.4804f, -146.2717f, 2.234021f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -485.1559f, -89.57419f, -146.9299f, 3.071779f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -482.2516f, -96.80614f, -146.6596f, 2.303835f},
    {6, NPC_CAVERNDEEP_AMBUSHER, -477.9874f, -92.82047f, -146.6944f, 3.124139f},

    // Grubbis and add
    {7, NPC_GRUBBIS, -476.3761f, -108.1901f, -145.7763f, 1.919862f},
    {7, NPC_CHOMPER, -473.1326f, -103.0901f, -146.1155f, 2.042035f}
};

// VMaNGOS script_waypoint entry 7998 (pointid, x, y, z, waittime ms)
struct ClassicGnomereganEscortPoint
{
    uint32 Id;
    float X, Y, Z;
    uint32 WaitMs;
};

ClassicGnomereganEscortPoint const EmiShortfusePath[] =
{
    {  1, -510.13f,  -132.69f,   -152.5f,   0 },
    {  2, -511.099f, -129.74f,   -153.845f, 0 },
    {  3, -511.79f,  -127.476f,  -155.551f, 0 },
    {  4, -512.969f, -124.926f,  -156.115f, 5000 },
    {  5, -513.972f, -120.236f,  -156.116f, 0 },
    {  6, -514.388f, -115.19f,   -156.117f, 0 },
    {  7, -514.304f, -111.478f,  -155.52f,  0 },
    {  8, -514.84f,  -107.663f,  -154.893f, 0 },
    {  9, -518.994f, -101.416f,  -154.648f, 27000 },
    { 10, -526.998f, -98.1488f,  -155.625f, 0 },
    { 11, -534.569f, -105.41f,   -155.989f, 30000 },
    { 12, -535.534f, -104.695f,  -155.971f, 0 },
    { 13, -541.63f,  -98.6583f,  -155.858f, 25000 },
    { 14, -535.092f, -99.9175f,  -155.974f, 0 },
    { 15, -519.01f,  -101.51f,   -154.677f, 3000 },
    { 16, -504.466f, -97.848f,   -150.955f, 30000 },
    { 17, -506.907f, -89.1474f,  -151.083f, 23000 },
    { 18, -512.758f, -101.902f,  -153.198f, 0 },
    { 19, -519.988f, -124.848f,  -156.128f, 86400000 }  // this npc should not reset on wp end
};
}

struct classic_npc_blastmaster_emi_shortfuse : public EscortAI
{
    classic_npc_blastmaster_emi_shortfuse(Creature* creature) : EscortAI(creature)
    {
        _instance = dynamic_cast<classic_instance_gnomeregan_InstanceScript*>(creature->GetInstanceScript());
        _phase = 0;
        _phaseTimer = 0;
        _didAggroText = false;
        _southernCaveInOpened = false;
        _northernCaveInOpened = false;
    }

    // VMaNGOS (constructor): remove Gossip-Menu in reload case for DONE encounter
    void JustAppeared() override
    {
        EscortAI::JustAppeared();
        if (_instance && _instance->GetData(TYPE_GRUBBIS) == DONE)
            me->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
    }

    void Reset() override
    {
        _didAggroText = false;                              // Used for 'defend' text, is triggered when the npc is attacked

        if (!HasEscortState(STATE_ESCORT_ESCORTING))
        {
            _phase = 0;
            _phaseTimer = 0;
            _southernCaveInOpened = _northernCaveInOpened = false;
            _summonedMobGUIDs.clear();
        }
    }

    void DoSummonPack(uint8 index)
    {
        for (ClassicGnomereganSummonInformation const& info : GnomereganSummonInfo)
        {
            // This requires order of the array
            if (info.Position > index)
                break;
            if (info.Position == index)
                me->SummonCreature(info.Entry, info.X, info.Y, info.Z, info.O, TEMPSUMMON_DEAD_DESPAWN);
        }
    }

    void JustSummoned(Creature* summoned) override
    {
        switch (summoned->GetEntry())
        {
            case NPC_CAVERNDEEP_BURROWER:
            case NPC_CAVERNDEEP_AMBUSHER:
            {
                if (!_instance)
                    break;
                if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(_phase > 20 ? GO_CAVE_IN_NORTH : GO_CAVE_IN_SOUTH)))
                {
                    float x, y, z;
                    door->GetNearPoint(door, x, y, z, 2.0f, frand(0.0f, 2 * float(M_PI)));
                    summoned->GetMotionMaster()->MovePoint(1, x, y, z);
                }
                break;
            }
            case NPC_CHOMPER: //chomper must be invoqued after grubbis
                // TODO(classic): VMaNGOS JoinCreatureGroup(grubbis, 3, angle, OPTION_FORMATION_MOVE | OPTION_AGGRO_TOGETHER);
                // TC creature formations need DB spawn ids, so Chomper only follows Grubbis here (no linked aggro).
                if (Creature* grubbis = summoned->FindNearestCreature(NPC_GRUBBIS, 10.0f, true))
                    summoned->GetMotionMaster()->MoveFollow(grubbis, 3.0f, (summoned->GetAbsoluteAngle(me) - me->GetOrientation()) + 2 * float(M_PI));
                break;
            default:
                break;
        }
        _summonedMobGUIDs.push_back(summoned->GetGUID());
    }

    void SummonedCreatureDies(Creature* summoned, Unit* /*killer*/) override
    {
        if (summoned->GetEntry() == NPC_GRUBBIS)
        {
            if (_instance)
                _instance->SetData(TYPE_GRUBBIS, DONE);
            _phaseTimer = 1000;
        }
        _summonedMobGUIDs.remove(summoned->GetGUID());
    }

    bool IsPreparingExplosiveCharge() const
    {
        return _phase == 11 || _phase == 13 || _phase == 26 || _phase == 28;
    }

    void MoveInLineOfSight(Unit* who) override
    {
        // In case we are preparing the explosive charges, we won't start attacking mobs
        if (IsPreparingExplosiveCharge())
            return;

        EscortAI::MoveInLineOfSight(who);
    }

    void AttackStart(Unit* who) override
    {
        // In case we are preparing the explosive charges, we won't start attacking mobs
        if (IsPreparingExplosiveCharge())
            return;

        EscortAI::AttackStart(who);
    }

    // VMaNGOS AttackedBy(): possibility for Aggro-Text only once per combat
    void JustEngagedWith(Unit* who) override
    {
        if (_didAggroText)
            return;

        _didAggroText = true;

        if (!urand(0, 2))
            ClassicScriptText(urand(0, 1) ? SAY_AGGRO_1 : SAY_AGGRO_2, me, who);
    }

    void JustDied(Unit* killer) override
    {
        EscortAI::JustDied(killer);

        if (!_instance)
            return;

        _instance->SetData(TYPE_GRUBBIS, FAIL);

        if (_southernCaveInOpened)                          // close southern cave-in door
            _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_SOUTH));
        if (_northernCaveInOpened)                          // close northern cave-in door
            _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_NORTH));

        for (ObjectGuid const& guid : _summonedMobGUIDs)
        {
            if (Creature* summoned = me->GetMap()->GetCreature(guid))
                summoned->DespawnOrUnsummon();
        }
    }

    void StartEvent(Player* player)
    {
        if (!_instance)
            return;

        _instance->SetData(TYPE_GRUBBIS, IN_PROGRESS);

        _phase = 1;
        _phaseTimer = 1000;
        _playerGuid = player->GetGUID();
    }

    bool OnGossipHello(Player* player) override
    {
        if (_instance)
        {
            if (_instance->GetData(TYPE_GRUBBIS) == NOT_STARTED || _instance->GetData(TYPE_GRUBBIS) == FAIL)
            {
                // TODO(classic): VMaNGOS uses broadcast text 4084 for the option text
                AddGossipItemFor(player, GossipOptionNpc::None, "I am ready to begin.", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
                SendGossipMenuFor(player, player->GetGossipTextId(me), me->GetGUID());
            }
        }
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        if (action == GOSSIP_ACTION_INFO_DEF + 1)
        {
            if (_instance)
            {
                if (_instance->GetData(TYPE_GRUBBIS) == NOT_STARTED || _instance->GetData(TYPE_GRUBBIS) == FAIL)
                    StartEvent(player);
            }
        }
        CloseGossipMenuFor(player);

        return true;
    }

    void WaypointStarted(uint32 waypointId, uint32 /*pathId*/) override
    {
        switch (waypointId)
        {
            case 10:
                // Open Southern Cave-In
                if (_instance && !_southernCaveInOpened)
                {
                    _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_SOUTH));
                    _southernCaveInOpened = true;
                }
                break;
            case 12:
                ClassicScriptText(SAY_CHARGE_1, me);
                break;
            case 16:
                ClassicScriptText(SAY_CHARGE_3, me);
                // Open Northern Cave-In
                if (_instance && !_northernCaveInOpened)
                {
                    _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_NORTH));
                    _northernCaveInOpened = true;
                }
                break;
            default:
                break;
        }
    }

    void WaypointReached(uint32 waypointId, uint32 /*pathId*/) override
    {
        switch (waypointId)
        {
            case 4:
                _phaseTimer = 1000;
                break;
            case 9:
                _phaseTimer = 2000;
                break;
            case 11:
                me->SetEmoteState(EMOTE_STATE_USE_STANDING);
                _phaseTimer = 15000;
                break;
            case 13:
                me->SetEmoteState(EMOTE_STATE_USE_STANDING);
                _phaseTimer = 10000;
                break;
            case 15:
                SetEscortPaused(true);
                if (_instance)
                {
                    if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(GO_CAVE_IN_SOUTH)))
                        me->SetFacingToObject(door);
                }
                ClassicScriptText(SAY_BLOW_1_10, me);
                _phaseTimer = 5000;
                break;
            case 16:
                me->SetEmoteState(EMOTE_STATE_USE_STANDING);
                _phaseTimer = 15000;
                break;
            case 17:
                me->SetEmoteState(EMOTE_STATE_USE_STANDING);
                _phaseTimer = 10000;
                break;
            case 19:
                _phaseTimer = 2000;
                SetEscortPaused(true);                      // And keep paused from now on!
                break;
            default:
                break;
        }
    }

    void UpdateEscortAI(uint32 diff) override
    {
        // Watchdog. The phases wait for the waypoints and the waypoints wait for the end of the fight: when she is left in
        // combat with nothing to attack (a trogg that never reached her) or the return after a fight never reports back, the
        // event stopped for good with the cave-in open (2026-10-11: she said "the final charge is ready", took one step
        // towards the last point and stood there; no explosion, no Grubbis). The longest planned stop is 30 s at a
        // waypoint, so 40 s without moving, without a victim and without a phase running is a stall: leave combat and
        // go on.
        if (HasEscortState(STATE_ESCORT_ESCORTING) && !HasEscortState(STATE_ESCORT_PAUSED) && !_phaseTimer && !me->GetVictim() && !me->isMoving()
            && _phase > 3 && _phase <= 31)
        {
            _stallTimer += diff;
            if (_stallTimer >= 40000)
            {
                _stallTimer = 0;
                TC_LOG_ERROR("scripts", "classic_npc_blastmaster_emi_shortfuse: stalled in phase {} (engaged {}, returning {}), leaving combat to go on",
                    uint32(_phase), me->IsEngaged(), HasEscortState(STATE_ESCORT_RETURNING));
                EscortAI::EnterEvadeMode(EvadeReason::Other);
            }
        }
        else
            _stallTimer = 0;

        // the phases are handled OOC (keeps them in sync with the waypoints)
        if (_phaseTimer && !me->GetVictim())
        {
            if (_phaseTimer <= diff)
            {
                switch (_phase)
                {
                    case 1:
                        ClassicScriptText(SAY_START, me);
                        me->SetFaction(FACTION_ESCORTEE_N_NEUTRAL_PASSIVE);  // VMaNGOS SetFactionTemporary(..., TEMPFACTION_RESTORE_RESPAWN)
                        me->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
                        me->ReplaceAllDynamicFlags(UNIT_DYNFLAG_NONE);
                        _phaseTimer = 5000;
                        break;
                    case 2:
                        ClassicScriptText(SAY_INTRO_1, me);
                        _phaseTimer = 3500;              // 6s delay, but 2500ms for escortstarting
                        break;
                    case 3:
                        // VMaNGOS Start(bRun = false, playerGuid, nullptr, false, false)
                        ResetPath();
                        for (ClassicGnomereganEscortPoint const& point : EmiShortfusePath)
                            AddWaypoint(point.Id, point.X, point.Y, point.Z, 0.0f, point.WaitMs ? Optional<Milliseconds>(Milliseconds(point.WaitMs)) : Optional<Milliseconds>(), false);
                        SetDespawnAtEnd(false);
                        Start(true, _playerGuid, nullptr, false, false);
                        _phaseTimer = 0;
                        break;

                    case 4:                                 // Shortly after reached WP 4
                        ClassicScriptText(SAY_INTRO_2, me);
                        _phaseTimer = 0;
                        break;

                    case 5:                                 // Shortly after reached WP 9
                        ClassicScriptText(SAY_INTRO_3, me);
                        _phaseTimer = 6000;
                        break;
                    case 6:
                        ClassicScriptText(SAY_INTRO_4, me);
                        _phaseTimer = 9000;
                        break;
                    case 7:
                        if (_instance)
                        {
                            if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(GO_CAVE_IN_SOUTH)))
                                me->SetFacingToObject(door);
                        }
                        _phaseTimer = 2000;
                        break;
                    case 8:
                        ClassicScriptText(SAY_LOOK_1, me);
                        _phaseTimer = 5000;
                        break;
                    case 9:
                        ClassicScriptText(SAY_HEAR_1, me);
                        _phaseTimer = 2000;
                        break;
                    case 10:                                // Shortly shortly before starting WP 11
                        DoSummonPack(1);
                        _phaseTimer = 0;
                        break;

                    case 11:                                // 15s after reached WP 11
                        DoSummonPack(2);

                        // Summon first explosive charge
                        if (_instance)
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_1);
                        // Remove EMOTE_STATE_USESTANDING state-emote
                        me->SetEmoteState(EMOTE_ONESHOT_NONE);

                        _phaseTimer = 1;
                        break;
                    case 12:                                // Empty Phase, used to store information about set charge
                        _phaseTimer = 0;
                        break;

                    case 13:                                // 10s after reached WP 13
                        DoSummonPack(3);

                        // Summon second explosive charge
                        if (_instance)
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_2);
                        // Remove EMOTE_STATE_USESTANDING state-emote
                        me->SetEmoteState(EMOTE_ONESHOT_NONE);

                        _phaseTimer = 11000;
                        break;
                    case 14:                                // Empty Phase, used to store information about set charge
                        _phaseTimer = 1;
                        break;
                    case 15:                                // shortly before starting WP 14
                        if (Player* player = ObjectAccessor::GetPlayer(*me, _playerGuid))
                            me->SetFacingToObject(player);
                        ClassicScriptText(SAY_CHARGE_2, me);
                        _phaseTimer = 0;
                        break;

                    case 16:                                // 5s after reaching WP 15
                        ClassicScriptText(SAY_BLOW_1_5, me);
                        _phaseTimer = 5000;
                        break;
                    case 17:
                        ClassicScriptText(SAY_BLOW_1, me);
                        _phaseTimer = 1000;
                        break;
                    case 18:
                        DoCastSelf(SPELL_EXPLOSION_SOUTH);
                        _phaseTimer = 500;
                        break;
                    case 19:
                        // Close southern cave-in and let charges explode
                        if (_instance)
                        {
                            _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_SOUTH));
                            _southernCaveInOpened = false;
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_USE);
                        }
                        _phaseTimer = 5000;
                        break;
                    case 20:
                        me->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
                        _phaseTimer = 6000;
                        break;
                    case 21:
                        ClassicScriptText(SAY_FINISH_1, me);
                        _phaseTimer = 6000;
                        break;
                    case 22:
                        ClassicScriptText(SAY_LOOK_2, me);
                        _phaseTimer = 3000;
                        break;
                    case 23:
                        if (_instance)
                        {
                            if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(GO_CAVE_IN_NORTH)))
                                me->SetFacingToObject(door);
                        }
                        _phaseTimer = 3000;
                        break;
                    case 24:
                        ClassicScriptText(SAY_HEAR_2, me);
                        _phaseTimer = 8000;
                        break;
                    case 25:                                // shortly before starting WP 16
                        SetEscortPaused(false);
                        DoSummonPack(4);
                        _phaseTimer = 0;
                        break;

                    case 26:                                // 15s after reaching WP 16
                        DoSummonPack(5);

                        // Summon third explosive charge
                        if (_instance)
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_3);
                        // Remove EMOTE_STATE_USESTANDING state-emote
                        me->SetEmoteState(EMOTE_ONESHOT_NONE);

                        _phaseTimer = 1;
                        break;
                    case 27:                                // Empty Phase, used to store information about set charge
                        _phaseTimer = 0;
                        break;

                    case 28:                                // 10s after reaching WP 17
                        DoSummonPack(6);

                        // Summon forth explosive charge
                        if (_instance)
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_4);
                        // Remove EMOTE_STATE_USESTANDING state-emote
                        me->SetEmoteState(EMOTE_ONESHOT_NONE);

                        _phaseTimer = 10000;
                        break;
                    case 29:                                // Empty Phase, used to store information about set charge
                        _phaseTimer = 1;
                        break;
                    case 30:                                // shortly before starting WP 18
                        ClassicScriptText(SAY_CHARGE_4, me);
                        _phaseTimer = 0;
                        break;

                    case 31:                                // shortly after reaching WP 19
                        if (_instance)
                        {
                            if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(GO_CAVE_IN_NORTH)))
                                me->SetFacingToObject(door);
                        }
                        ClassicScriptText(SAY_BLOW_2_10, me);
                        _phaseTimer = 5000;
                        break;
                    case 32:
                        ClassicScriptText(SAY_BLOW_2_5, me);
                        _phaseTimer = 1000;
                        break;
                    case 33:
                        DoSummonPack(7);                    // Summon Grubbis and add
                        _phaseTimer = 1000;
                        break;
                    case 34:
                        if (_instance)
                        {
                            if (Creature* grubbis = me->FindNearestCreature(NPC_GRUBBIS, 100.0f))
                                ClassicScriptText(SAY_GRUBBIS_SPAWN, grubbis);
                        }
                        _phaseTimer = 0;
                        break;
                    case 35:                                // 1 sek after Death of Grubbis
                        if (_instance)
                        {
                            if (GameObject* door = me->GetMap()->GetGameObject(_instance->GetGuidData(GO_CAVE_IN_NORTH)))
                                me->SetFacingToObject(door);
                        }
                        me->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
                        _phaseTimer = 5000;
                        break;
                    case 36:
                        ClassicScriptText(SAY_BLOW_SOON, me);
                        _phaseTimer = 5000;
                        break;
                    case 37:
                        ClassicScriptText(SAY_BLOW_2, me);
                        _phaseTimer = 2000;
                        break;
                    case 38:
                        me->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                        _phaseTimer = 1000;
                        break;
                    case 39:
                        DoCastSelf(SPELL_EXPLOSION_NORTH);
                        _phaseTimer = 500;
                        break;
                    case 40:
                        // Close northern cave-in and let charges explode
                        if (_instance)
                        {
                            _instance->DoUseDoorOrButton(_instance->GetGuidData(GO_CAVE_IN_NORTH));
                            _northernCaveInOpened = false;
                            _instance->SetData(TYPE_EXPLOSIVE_CHARGE, DATA_EXPLOSIVE_CHARGE_USE);
                        }
                        _phaseTimer = 8000;
                        break;
                    case 41:
                        DoCastSelf(SPELL_FIREWORKS_RED);
                        ClassicScriptText(SAY_FINISH_2, me);
                        _phaseTimer = 0;
                        break;
                    default:
                        break;
                }
                ++_phase;
            }
            else
                _phaseTimer -= diff;
        }

        UpdateVictim();
    }

private:
    classic_instance_gnomeregan_InstanceScript* _instance;

    uint8 _phase;
    uint32 _phaseTimer;
    uint32 _stallTimer = 0;
    ObjectGuid _playerGuid;
    bool _didAggroText, _southernCaveInOpened, _northernCaveInOpened;
    std::list<ObjectGuid> _summonedMobGUIDs;
};

/*######
## npc_kernobee
## TODO: It appears there are some things missing, including his? alarm-bot
######*/

enum Kernobee
{
    QUEST_A_FINE_MESS           = 2904,
    TRIGGER_GNOME_EXIT          = 324,                      // Add scriptlib support for it, atm simply use hardcoded values

    SPELL_EXPLOSION             = 27745, //spell de Clank.

    SAY_KERNOBEE_START          = 3881,
    SAY_BOMB_START              = 3927,
    SAY_BOMB_SEE_END            = 3949,
    SAY_KERNOBEE_SEE_END        = 3948,
    SAY_KERNOBEE_END            = 3929
};

namespace
{
float const KernobeePositions[3][3] =
{
    { -390.82f, 42.34f, -154.795f},                         // I can see the end!
    { -330.92f, -3.03f, -152.85f},                          // End position
    { -297.32f, -7.32f, -152.85f}                           // Walk out of the door
};
}

struct classic_npc_kernobee : public FollowerAI
{
    classic_npc_kernobee(Creature* creature) : FollowerAI(creature)
    {
        _checkEndposTimer = 10000;
        _nextStepTimer = 2000;
        _nextStep = 0;
        _canSeeEnd = false;
    }

    void Reset() override { }

    // VMaNGOS JustRespawned (+ QuestReset() from the AI constructor)
    void JustAppeared() override
    {
        me->SetImmuneToNPC(true);
        FollowerAI::JustAppeared();
        QuestReset();
    }

    void UpdateFollowerAI(uint32 diff) override
    {
        FollowerAI::UpdateFollowerAI(diff);                 // Do combat handling
        if (_nextStep == 5) //HasFollowState(STATE_FOLLOW_COMPLETE)
        {
            if (_nextStepTimer < diff)
            {
                if (Creature* creature = me->GetMap()->GetCreature(_bombGuid))
                {
                    creature->CastSpell(creature, SPELL_EXPLOSION, true);
                    _nextStep = 6;
                    _nextStepTimer = 200;
                }
            }
            else
                _nextStepTimer -= diff;
        }
        else if (_nextStep == 6) //HasFollowState(STATE_FOLLOW_COMPLETE)
        {
            if (_nextStepTimer < diff)
            {
                if (Creature* creature = me->GetMap()->GetCreature(_bombGuid))
                {
                    _nextStep = 0;
                    creature->DisappearAndDie();
                    me->DisappearAndDie();
                    return;
                }
            }
            else
                _nextStepTimer -= diff;
        }

        if (me->IsInCombat() || !HasFollowState(STATE_FOLLOW_INPROGRESS) || HasFollowState(STATE_FOLLOW_COMPLETE))
            return;

        if (_nextStep == 1) /*HasFollowState(STATE_FOLLOW_PAUSED)*/
        {
            if (_nextStepTimer < diff)
            {
                if (me->FindNearestCreature(NPC_ALARM_A_BOMB_2600, 10.0f)) // ALARM-A-BOMB must always be near escort NPC
                {
                    me->SetWalk(true); // speed influences speed of follower
                    SetFollowPaused(false);
                    _nextStepTimer = 5 * MINUTE * IN_MILLISECONDS;
                    _nextStep = 2;
                }
                else
                    _nextStepTimer = 500;
            }
            else
                _nextStepTimer -= diff;
        }
        if (_nextStep == 2)
        {
            if (_nextStepTimer < diff)
            {
                if (Creature* creature = me->GetMap()->GetCreature(_bombGuid))
                {
                    creature->CastSpell(creature, SPELL_EXPLOSION, true);
                    _nextStep = 6; //skip to they both die.
                    _nextStepTimer = 200;
                }
            }
            else
                _nextStepTimer -= diff;
        }
        if (_checkEndposTimer < diff)
        {
            if (_nextStep == 2) /*!explosionTimerStarted*/
            {
                if (Creature* bomb = me->GetMap()->GetCreature(_bombGuid))
                {
                    if (bomb->IsWithinDist3d(KernobeePositions[0][0], KernobeePositions[0][1], KernobeePositions[0][2], 2 * INTERACTION_DISTANCE))
                    {
                        _nextStep = 3;
                        _nextStepTimer = 20000;
                        ClassicScriptText(SAY_BOMB_SEE_END, bomb);
                    }
                }
            }
            if (!_canSeeEnd)
            {
                if (me->IsWithinDist3d(KernobeePositions[0][0], KernobeePositions[0][1], KernobeePositions[0][2], 2 * INTERACTION_DISTANCE))
                {
                    ClassicScriptText(SAY_KERNOBEE_SEE_END, me);
                    _canSeeEnd = true;
                }
            }
            else
            {
                if (me->IsWithinDist3d(KernobeePositions[1][0], KernobeePositions[1][1], KernobeePositions[1][2], 2 * INTERACTION_DISTANCE))
                {
                    ClassicScriptText(SAY_KERNOBEE_END, me);
                    _nextStep = 5;
                    _nextStepTimer = 2500;
                    SetFollowComplete(true);
                    if (Player* player = GetLeaderForFollower())
                        player->GroupEventHappens(QUEST_A_FINE_MESS, me);
                }
            }
            _checkEndposTimer = 200;
        }
        else
            _checkEndposTimer -= diff;
        if (_nextStep == 3)
        {
            if (_nextStepTimer < diff)
            {
                if (Creature* bomb = me->GetMap()->GetCreature(_bombGuid))
                {
                    bomb->CastSpell(bomb, SPELL_EXPLOSION, true);
                    _nextStep = 4;
                    _nextStepTimer = 200;
                    Unit::Kill(bomb, me);                   // VMaNGOS DealDamage(me, me->GetHealth())
                    if (bomb->IsAlive())
                        Unit::Kill(bomb, bomb);
                    return;
                }
            }
            else
                _nextStepTimer -= diff;
        }
        if (_nextStep == 4)
        {
            if (_nextStepTimer < diff)
            {
                if (Creature* bomb = me->GetMap()->GetCreature(_bombGuid))
                {
                    if (bomb->IsAlive())
                        Unit::Kill(bomb, bomb);
                    _nextStep = 0;
                    if (me->IsAlive())
                        Unit::Kill(bomb, me);
                }
            }
            else
                _nextStepTimer -= diff;
        }
    }

    void JustDied(Unit* killer) override
    {
        FollowerAI::JustDied(killer);
        QuestReset();
        if (Creature* bomb = me->GetMap()->GetCreature(_bombGuid))
            bomb->DisappearAndDie();
    }

    void QuestReset()
    {
        me->SetStandState(UNIT_STAND_STATE_DEAD);
        _checkEndposTimer = 10000;
        _nextStepTimer = 2000;
        _canSeeEnd = false;
        _nextStep = 0;
    }

    void OnQuestAccept(Player* player, Quest const* quest) override
    {
        if (quest->GetQuestId() != QUEST_A_FINE_MESS)
            return;

        ClassicScriptText(SAY_KERNOBEE_START, me);
        me->SetStandState(UNIT_STAND_STATE_STAND);
        me->SetImmuneToNPC(false);
        StartFollow(player, 0/*FACTION_ESCORT_N_FRIEND_PASSIVE*/, quest->GetQuestId());
        InstanceScript* instance = me->GetInstanceScript();
        _bombGuid = instance ? instance->GetGuidData(NPC_ALARM_A_BOMB_2600) : ObjectGuid::Empty;
        if (!_bombGuid.IsEmpty())
        {
            if (Creature* bomb = me->GetMap()->GetCreature(_bombGuid))
            {
                SetFollowPaused(true);
                me->SetWalk(false);
                bomb->GetMotionMaster()->MoveFollow(me, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
                ClassicScriptText(SAY_BOMB_START, bomb);
            }
        }
        _nextStep = 1;
    }

private:
    uint32 _checkEndposTimer;
    uint32 _nextStepTimer;
    uint16 _nextStep;
    ObjectGuid _bombGuid;
    bool _canSeeEnd;
};

// 12709 - Collecting Fallout (Heavy Leaden Collection Phial)
class classic_spell_gnomeregan_collecting_fallout : public SpellScript
{
    bool Load() override
    {
        _chosenEffect = urand(EFFECT_0, EFFECT_1);
        return true;
    }

    // only execute one of the trigger spell effects
    void HandleEffect(SpellEffIndex effIndex)
    {
        if (uint32(effIndex) != _chosenEffect)
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        OnEffectLaunch += SpellEffectFn(classic_spell_gnomeregan_collecting_fallout::HandleEffect, EFFECT_ALL, SPELL_EFFECT_ANY);
        OnEffectLaunchTarget += SpellEffectFn(classic_spell_gnomeregan_collecting_fallout::HandleEffect, EFFECT_ALL, SPELL_EFFECT_ANY);
        OnEffectHit += SpellEffectFn(classic_spell_gnomeregan_collecting_fallout::HandleEffect, EFFECT_ALL, SPELL_EFFECT_ANY);
        OnEffectHitTarget += SpellEffectFn(classic_spell_gnomeregan_collecting_fallout::HandleEffect, EFFECT_ALL, SPELL_EFFECT_ANY);
    }

    uint32 _chosenEffect = 0;
};

void AddSC_classic_gnomeregan()
{
    RegisterCreatureAI(classic_npc_blastmaster_emi_shortfuse);
    RegisterCreatureAI(classic_npc_kernobee);
    RegisterSpellScript(classic_spell_gnomeregan_collecting_fallout);
}
