/*
 * Forever bots: class and spec knowledge (Classic 1.60 spells, by name: the spell book keeps the best rank).
 *
 * The utility layer already handles interrupts, dispels, taunts, crowd control, defensives, buffs and resurrection
 * from the classified spell book; this layer adds what needs class knowledge: stances, forms, aspects, seals, auras,
 * armors, weapon imbues, totems, pets, and the damage / threat priorities of each class and spec. The spec is read
 * from the talent spells the bot knows (Mortal Strike = Arms, Shadowform = Shadow...).
 */

#include "BotAI.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

using namespace ForeverBots;

bool ForeverBotAI::IsRangedClass() const
{
    return me->GetClass() == CLASS_HUNTER;
}

bool ForeverBotAI::IsCasterClass() const
{
    ForeverBot const* data = GetBotData();
    BotRole const role = data ? (data->IsWorldBot() ? BotRole::Dps : data->Role) : BotRole::Dps;
    switch (me->GetClass())
    {
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            return true;
        case CLASS_DRUID:
            // a feral druid fights in melee
            return role == BotRole::Heal || (role == BotRole::Dps && !_spells.Find("Cat Form"));
        case CLASS_SHAMAN:
            return role == BotRole::Heal || (role == BotRole::Dps && !_spells.Find("Stormstrike"));
        case CLASS_PALADIN:
            return role == BotRole::Heal;
        default:
            return false;
    }
}

float ForeverBotAI::PreferredRange() const
{
    if (IsRangedClass())
        return 30.0f;
    return IsCasterClass() ? 28.0f : 0.0f;
}

// A hunter bot without a pet: calls the one it has (Call Pet), revives it when it died and its body is gone, or gets a
// wolf of its own level the first time, the way ".pet create" does (nobody tames a beast for a bot).
void ForeverBotAI::EnsureHunterPet()
{
    uint32 const now = GameTime::GetGameTimeMS();
    if (now < _petRetry)
        return;
    _petRetry = now + 8000;

    // the taming quest chain teaches these; a bot never did it
    if (!me->HasSpell(883) || !me->HasSpell(982))
    {
        for (uint32 spellId : { 883u, 982u })       // Call Pet, Revive Pet
            if (!me->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
                me->LearnSpell(spellId, false);
        _petRetry = now + 3000;                     // the spell book picks them up on its next rebuild
    }

    if (PetStable const* stable = me->GetPetStable(); stable && stable->GetCurrentPet())
    {
        // a pet in the stable: alive -> Call Pet, dead -> Revive Pet (whichever the server accepts)
        if (BotSpell const* call = _spells.Find("Call Pet"); call && Cast(call, me))
            return;
        if (BotSpell const* revive = _spells.Find("Revive Pet"))
            Cast(revive, me);
        return;
    }

    static constexpr uint32 PetEntry = 2958;        // Prairie Wolf: a wolf (bite), tameable in every Classic data set
    Pet* pet = me->CreateTamedPetFrom(PetEntry, 13481);
    if (!pet)
        return;
    pet->GetMap()->AddToMap(pet->ToCreature());
    me->SetMinion(pet, true);
    if (pet->HasHappiness())
        pet->SetPower(POWER_HAPPINESS, Pet::HAPPINESS_MAX);
    pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    me->PetSpellInitialize();
    TC_LOG_INFO("server.foreverbots", "ForeverBots: {} (hunter, level {}) got a pet", me->GetName(), me->GetLevel());
}

bool ForeverBotAI::ProposeNamed(int32 relevance, char const* name, Unit* target, char const* reason)
{
    BotSpell const* spell = _spells.Find(name);
    if (!spell || !target || !CanCast(spell, target))
        return false;
    _brain.Propose(relevance, spell->Name.c_str(), target, [this, spell, target]() { return Cast(spell, target); }, reason);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Upkeep: stances, forms, aspects, seals, auras, armors, imbues, totems, pets
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeClassUpkeep(BotContext& ctx, Unit* target, bool inCombat)
{
    BotRole const role = EffectiveRole(ctx);
    auto missing = [this](char const* name) { return _spells.Find(name) && !HasAuraNamed(me, name, true); };
    auto firstKnown = [this](std::initializer_list<char const*> names) -> char const*
    {
        for (char const* name : names)
            if (_spells.Find(name))
                return name;
        return nullptr;
    };
    auto keepOne = [&](int32 relevance, std::initializer_list<char const*> names, char const* reason)
    {
        char const* wanted = firstKnown(names);
        if (wanted && !HasAuraNamed(me, wanted, true))
            ProposeNamed(relevance, wanted, me, reason);
    };

    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            if (role == BotRole::Tank)
                keepOne(R_TAUNT + 5, { "Defensive Stance" }, "tank stance");
            else if (_spells.Find("Berserker Stance") && !inCombat)
                keepOne(R_BUFF + 2, { "Berserker Stance" }, "damage stance");
            else if (!HasAuraNamed(me, "Berserker Stance", true))
                keepOne(R_ROTATION + 18, { "Battle Stance" }, "damage stance");
            if (inCombat || ctx.AutonomousLeader)
                if (missing("Battle Shout"))
                    ProposeNamed(R_ROTATION + 17, "Battle Shout", me, "shout missing");
            break;
        case CLASS_PALADIN:
            if (role == BotRole::Tank)
                keepOne(R_BUFF + 3, { "Righteous Fury" }, "tank threat");
            keepOne(R_BUFF + 2, role == BotRole::Dps ? std::initializer_list<char const*>{ "Retribution Aura", "Devotion Aura" }
                : std::initializer_list<char const*>{ "Devotion Aura" }, "aura");
            if (inCombat && role != BotRole::Heal && !HasAuraNamed(me, "Seal of Command", true) && !HasAuraNamed(me, "Seal of Righteousness", true))
                keepOne(R_ROTATION + 19, { "Seal of Command", "Seal of Righteousness" }, "seal");
            if (!inCombat)
            {
                // one blessing per paladin per member: Might for fighters, Wisdom for mana users
                for (Player* member : ctx.Members)
                {
                    bool const manaUser = member->GetPowerType() == POWER_MANA && member->GetClass() != CLASS_HUNTER && member->GetClass() != CLASS_PALADIN;
                    char const* blessing = manaUser && _spells.Find("Blessing of Wisdom") ? "Blessing of Wisdom" : "Blessing of Might";
                    if (_spells.Find(blessing) && !HasAuraNamed(member, blessing, true))
                    {
                        ProposeNamed(R_BUFF - 1, blessing, member, "blessing");
                        break;
                    }
                }
            }
            break;
        case CLASS_HUNTER:
            if (!HasAuraNamed(me, "Aspect of the Hawk", true) && !HasAuraNamed(me, "Aspect of the Monkey", true))
                keepOne(R_BUFF + 2, { "Aspect of the Hawk", "Aspect of the Monkey" }, "aspect");
            else if (_spells.Find("Aspect of the Hawk") && HasAuraNamed(me, "Aspect of the Monkey", true))
                keepOne(R_BUFF + 2, { "Aspect of the Hawk" }, "better aspect");
            // the pet is most of a hunter's damage (LegionMind lesson: "all the ranged are weak" was hunters without a pet)
            if (Pet* pet = me->GetPet())
            {
                if (!pet->IsAlive())
                {
                    if (!inCombat && !me->isMoving())
                        ProposeNamed(R_BUFF + 4, "Revive Pet", me, "pet dead");
                }
                else
                {
                    if (pet->HasHappiness() && pet->GetPower(POWER_HAPPINESS) < Pet::HAPPINESS_MAX)
                        pet->SetPower(POWER_HAPPINESS, Pet::HAPPINESS_MAX);     // nobody feeds a bot's pet
                    if (pet->GetHealthPct() < 50.0f && !HasAuraNamed(pet, "Mend Pet", true))
                        ProposeNamed(inCombat ? R_ROTATION + 15 : R_BUFF + 3, "Mend Pet", pet, "pet hurt");
                }
            }
            else if (!inCombat && !me->isMoving() && !me->IsMounted())
                EnsureHunterPet();
            break;
        case CLASS_PRIEST:
            if (role == BotRole::Dps && _spells.Find("Shadowform"))
                keepOne(R_ROTATION + 20, { "Shadowform" }, "shadow spec");
            break;
        case CLASS_SHAMAN:
        {
            keepOne(R_BUFF + 1, { "Lightning Shield" }, "elemental shield");
            // weapon imbue on the main hand
            if (!inCombat)
                if (Item* weapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND); weapon && !weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                    if (char const* imbue = firstKnown({ "Windfury Weapon", "Flametongue Weapon", "Rockbiter Weapon" }))
                        ProposeNamed(R_BUFF + 1, imbue, me, "weapon imbue");
            // totems during a fight: earth for the group, fire for damage, water for the healer
            if (inCombat && target)
            {
                auto noTotem = [this](uint8 slot) { return me->m_SummonSlot[slot].IsEmpty(); };
                if (noTotem(SUMMON_SLOT_TOTEM_2))
                    if (char const* earth = firstKnown({ "Strength of Earth Totem", "Stoneskin Totem" }))
                        ProposeNamed(R_ROTATION + 16, earth, me, "earth totem");
                if (noTotem(SUMMON_SLOT_TOTEM) && role != BotRole::Heal && me->IsWithinDistInMap(target, 15.0f))
                    if (char const* fire = firstKnown({ "Searing Totem" }))
                        ProposeNamed(R_ROTATION + 15, fire, me, "fire totem");
                if (noTotem(SUMMON_SLOT_TOTEM_3))
                    if (char const* water = firstKnown(role == BotRole::Heal ? std::initializer_list<char const*>{ "Mana Spring Totem", "Healing Stream Totem" }
                        : std::initializer_list<char const*>{ "Healing Stream Totem" }))
                        ProposeNamed(R_ROTATION + 14, water, me, "water totem");
            }
            break;
        }
        case CLASS_MAGE:
            keepOne(R_BUFF + 2, { "Mage Armor", "Ice Armor", "Frost Armor" }, "armor");
            break;
        case CLASS_WARLOCK:
            keepOne(R_BUFF + 2, { "Demon Armor", "Demon Skin" }, "armor");
            // a demon: the Voidwalker when nobody tanks, otherwise the Imp
            if (!inCombat && !me->GetPet() && !me->isMoving())
            {
                // the Voidwalker costs a Soul Shard (6265): without one the cast is refused for reagents, the Imp is free
                bool const shard = me->HasItemCount(6265);
                char const* demon = !ctx.Tank && shard ? firstKnown({ "Summon Voidwalker", "Summon Imp" }) : firstKnown({ "Summon Imp" });
                if (demon)
                    ProposeNamed(R_BUFF + 3, demon, me, "no demon");
            }
            break;
        case CLASS_DRUID:
            if (role == BotRole::Tank)
            {
                if (!HasAuraNamed(me, "Dire Bear Form", true) && !HasAuraNamed(me, "Bear Form", true))
                    keepOne(R_TAUNT + 5, { "Dire Bear Form", "Bear Form" }, "tank form");
            }
            else if (role == BotRole::Dps && _spells.Find("Cat Form") && (inCombat || ctx.Members.size() > 1))
                keepOne(R_ROTATION + 20, { "Cat Form" }, "feral damage");
            break;
        default:
            break;
    }

    // the pet attacks what I attack
    if (inCombat && target)
        if (Pet* pet = me->GetPet(); pet && pet->IsAlive() && pet->GetVictim() != target && pet->IsAIEnabled())
            pet->AI()->AttackStart(target);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rotations
// ---------------------------------------------------------------------------------------------------------------------
void ForeverBotAI::ProposeClassRotation(BotContext& ctx, Unit* target, int32 offset)
{
    BotRole const role = EffectiveRole(ctx);
    float const targetHp = target->GetHealthPct();
    uint32 const near = CountEnemiesNear(ctx, target, 8.0f);
    uint32 const aroundMe = CountEnemiesNear(ctx, me, 8.0f);
    bool const inMelee = me->IsWithinMeleeRange(target);
    auto R = [offset](int32 priority) { return R_ROTATION + priority + offset; };
    auto missingOn = [this, target](char const* name) { return _spells.Find(name) && !HasAuraNamed(target, name, true); };
    auto power = [this](Powers p) { return me->GetPower(p); };
    int32 const rage = power(POWER_RAGE) / 10;          // the client shows rage / 10
    int32 const energy = power(POWER_ENERGY);
    int32 const combo = power(POWER_COMBO_POINTS);
    float const manaPct = me->GetMaxPower(POWER_MANA) ? me->GetPowerPct(POWER_MANA) : 100.0f;

    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            if (role == BotRole::Tank)
            {
                ProposeNamed(R(19), "Shield Slam", target, "threat");
                ProposeNamed(R(18), "Revenge", target, "threat (proc)");
                if (aroundMe >= 2)
                {
                    if (missingOn("Thunder Clap"))
                        ProposeNamed(R(17), "Thunder Clap", me, "area threat");
                    if (missingOn("Demoralizing Shout"))
                        ProposeNamed(R(15), "Demoralizing Shout", me, "area threat");
                    if (rage >= 35)
                        ProposeNamed(R(14), "Cleave", target, "rage dump, several mobs");
                }
                ProposeNamed(R(16), "Sunder Armor", target, "threat");
                if (rage >= 45)
                    ProposeNamed(R(13), "Heroic Strike", target, "rage dump");
            }
            else
            {
                if (!target->IsInCombat() || me->GetDistance(target) > 10.0f)
                    ProposeNamed(R(20), "Charge", target, "opener");
                if (targetHp < 20.0f)
                    ProposeNamed(R(19), "Execute", target, "execute");
                ProposeNamed(R(18), "Overpower", target, "proc");
                ProposeNamed(R(17), "Mortal Strike", target, "arms");
                ProposeNamed(R(17), "Bloodthirst", target, "fury");
                if (near >= 2)
                    ProposeNamed(R(16), "Whirlwind", target, "several mobs");
                if (missingOn("Rend") && targetHp > 50.0f)
                    ProposeNamed(R(12), "Rend", target, "bleed");
                if (rage >= 50)
                    ProposeNamed(R(11), near >= 2 && _spells.Find("Cleave") ? "Cleave" : "Heroic Strike", target, "rage dump");
            }
            break;
        case CLASS_PALADIN:
            ProposeNamed(R(16), "Judgement", target, "seal up");
            if (aroundMe >= 2 && manaPct > 40.0f)
                ProposeNamed(R(15), "Consecration", me, "several mobs");
            if (role != BotRole::Heal)
            {
                ProposeNamed(R(14), "Exorcism", target, "undead / demon");
                ProposeNamed(R(13), "Holy Strike", target, "strike");
                ProposeNamed(R(12), "Crusader Strike", target, "strike");
            }
            if (targetHp < 20.0f)
                ProposeNamed(R(18), "Hammer of Wrath", target, "execute");
            break;
        case CLASS_HUNTER:
        {
            // auto shot runs on its own timer: start it, don't queue it
            // By id, with the refusal logged: in the bot-only Wailing Caverns test the hunters used their stings and shots
            // but Auto Shot never started (no attempt and no refusal in the log), and it is most of their own damage.
            // The running Auto Shot keeps the target it was started on: when that one died, every shot failed its check in
            // silence (Unit::_UpdateAutoRepeatSpell) and, being "on", it was never started again - a hunter shot its first
            // mob and nothing after it (3 dps in a 5 minute run). It is restarted on the current target.
            if (Spell const* running = me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                if (inMelee || running->m_targets.GetUnitTargetGUID() != target->GetGUID())
                    me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            if (!inMelee && !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !me->HasUnitState(UNIT_STATE_CASTING) && !me->isMoving())
            {
                static constexpr uint32 SpellAutoShot = 75;
                if (!me->HasSpell(SpellAutoShot) && sSpellMgr->GetSpellInfo(SpellAutoShot, DIFFICULTY_NONE))
                    me->LearnSpell(SpellAutoShot, false);
                uint32 const now = GameTime::GetGameTimeMS();
                if (me->HasSpell(SpellAutoShot) && now >= _autoShotRetry)
                {
                    if (!me->HasInArc(float(M_PI) / 2, target))
                        me->SetFacingToObject(target);
                    SpellCastResult const result = me->CastSpell(target, SpellAutoShot, false);
                    if (result != SPELL_CAST_OK)
                    {
                        _autoShotRetry = now + 1500;
                        if (ctx.Data->Debug || sForeverBotMgr->IsGlobalDebug())
                            TC_LOG_INFO("server.foreverbots", "[BotAI] Bot={} Auto Shot on {} refused: {} (dist {:.1f})", me->GetName(), target->GetName(),
                                uint32(result), me->GetDistance(target));
                    }
                }
            }
            if (inMelee)
            {
                ProposeNamed(R(18), "Mongoose Bite", target, "melee proc");
                ProposeNamed(R(17), "Raptor Strike", target, "melee");
                ProposeNamed(R(10), "Wing Clip", target, "get away");
                break;
            }
            if (missingOn("Hunter's Mark") && targetHp > 50.0f)
                ProposeNamed(R(19), "Hunter's Mark", target, "mark");
            ProposeNamed(R(18), "Aimed Shot", target, "big shot");
            if (near >= 2)
                ProposeNamed(R(17), "Multi-Shot", target, "several mobs");
            if (missingOn("Serpent Sting") && targetHp > 40.0f)
                ProposeNamed(R(16), "Serpent Sting", target, "sting");
            if (manaPct > 30.0f)
                ProposeNamed(R(14), "Arcane Shot", target, "shot");
            break;
        }
        case CLASS_ROGUE:
        {
            bool const behind = !target->HasInArc(float(M_PI), me);
            if (combo >= 1 && !HasAuraNamed(me, "Slice and Dice", true) && targetHp > 40.0f)
                ProposeNamed(R(19), "Slice and Dice", target, "haste");      // a finisher: it needs the combo target
            if (combo >= 5 || (combo >= 2 && targetHp < 25.0f))
                ProposeNamed(R(18), "Eviscerate", target, "finisher");
            if (combo < 5)
            {
                if (behind)
                    ProposeNamed(R(17), "Backstab", target, "behind the target");
                ProposeNamed(R(15), energy >= 60 && _spells.Find("Hemorrhage") ? "Hemorrhage" : "Sinister Strike", target, "combo builder");
            }
            break;
        }
        case CLASS_PRIEST:
            if (missingOn("Shadow Word: Pain") && targetHp > 30.0f)
                ProposeNamed(R(18), "Shadow Word: Pain", target, "dot");
            if (missingOn("Vampiric Embrace"))
                ProposeNamed(R(17), "Vampiric Embrace", target, "shadow");
            ProposeNamed(R(16), "Mind Blast", target, "nuke");
            ProposeNamed(R(14), "Mind Flay", target, "channel");
            if (!HasAuraNamed(me, "Shadowform", true))
            {
                ProposeNamed(R(13), "Holy Fire", target, "nuke");
                ProposeNamed(R(12), "Smite", target, "filler");
            }
            break;
        case CLASS_SHAMAN:
            ProposeNamed(R(19), "Stormstrike", target, "enhancement");
            if (missingOn("Flame Shock") && targetHp > 30.0f)
                ProposeNamed(R(17), "Flame Shock", target, "dot");
            ProposeNamed(R(15), "Earth Shock", target, "shock");
            if (near >= 2)
                ProposeNamed(R(16), "Chain Lightning", target, "several mobs");
            if (!inMelee || role == BotRole::Heal)
                ProposeNamed(R(13), "Lightning Bolt", target, "filler");
            break;
        case CLASS_MAGE:
        {
            Unit* onMe = nullptr;
            for (Unit* enemy : ctx.Enemies)
                if (enemy->GetVictim() == me && me->IsWithinMeleeRange(enemy))
                    onMe = enemy;
            if (onMe)
                ProposeNamed(R(22), "Frost Nova", me, "melee on me");
            if (aroundMe >= 3)
                ProposeNamed(R(20), "Arcane Explosion", me, "mobs around me");
            if (near >= 3)
                if (BotSpell const* blizzard = _spells.Find("Blizzard"); blizzard && CanCast(blizzard, me, false))
                    _brain.Propose(R(19), blizzard->Name.c_str(), target, [this, blizzard, target]() { return CastAt(blizzard, target->GetPosition()); }, "pack");
            if (manaPct < 12.0f && !onMe)
                ProposeNamed(R(21), "Evocation", me, "out of mana");
            ProposeNamed(R(18), "Fire Blast", target, "instant");
            ProposeNamed(R(17), "Pyroblast", target, "fire spec");
            ProposeNamed(R(16), "Arcane Missiles", target, "arcane");
            ProposeNamed(R(15), _spells.Find("Frostbolt") ? "Frostbolt" : "Fireball", target, "nuke");
            break;
        }
        case CLASS_WARLOCK:
            if (manaPct < 20.0f && me->GetHealthPct() > 70.0f)
                ProposeNamed(R(20), "Life Tap", me, "mana");
            if (me->GetHealthPct() < 45.0f)
                ProposeNamed(R(19), "Drain Life", target, "heal myself");
            if (missingOn("Corruption") && targetHp > 30.0f)
                ProposeNamed(R(18), "Corruption", target, "dot");
            if (missingOn("Curse of Agony") && !HasAuraNamed(target, "Curse of the Elements", true) && targetHp > 30.0f)
                ProposeNamed(R(17), "Curse of Agony", target, "curse");
            if (missingOn("Immolate") && targetHp > 40.0f)
                ProposeNamed(R(16), "Immolate", target, "dot");
            if (near >= 3 && manaPct > 40.0f)
                if (BotSpell const* rain = _spells.Find("Rain of Fire"); rain && CanCast(rain, me, false))
                    _brain.Propose(R(15), rain->Name.c_str(), target, [this, rain, target]() { return CastAt(rain, target->GetPosition()); }, "pack");
            ProposeNamed(R(14), "Shadow Bolt", target, "nuke");
            break;
        case CLASS_DRUID:
            if (HasAuraNamed(me, "Bear Form", true) || HasAuraNamed(me, "Dire Bear Form", true))
            {
                if (aroundMe >= 2)
                {
                    ProposeNamed(R(18), "Swipe", target, "area threat");
                    if (missingOn("Demoralizing Roar"))
                        ProposeNamed(R(16), "Demoralizing Roar", me, "area threat");
                }
                ProposeNamed(R(17), "Maul", target, "threat");
                ProposeNamed(R(12), "Enrage", me, "rage");
            }
            else if (HasAuraNamed(me, "Cat Form", true))
            {
                if (combo >= 4)
                    ProposeNamed(R(19), missingOn("Rip") && targetHp > 40.0f ? "Rip" : "Ferocious Bite", target, "finisher");
                if (missingOn("Rake"))
                    ProposeNamed(R(17), "Rake", target, "bleed");
                ProposeNamed(R(16), "Shred", target, "behind");
                ProposeNamed(R(15), "Claw", target, "combo builder");
            }
            else
            {
                if (missingOn("Moonfire") && targetHp > 30.0f)
                    ProposeNamed(R(17), "Moonfire", target, "dot");
                if (missingOn("Insect Swarm"))
                    ProposeNamed(R(16), "Insect Swarm", target, "dot");
                ProposeNamed(R(15), "Starfire", target, "nuke");
                ProposeNamed(R(14), "Wrath", target, "filler");
            }
            break;
        default:
            break;
    }
}

// whatever damage spells the bot knows, when the class list has nothing castable (low levels, unknown specs)
void ForeverBotAI::ProposeGenericRotation(BotContext& ctx, Unit* target, int32 offset)
{
    BotSpell const* best = nullptr;
    float bestRate = 0.0f;
    for (BotSpell const* spell : _spells.Of(KIND_DAMAGE | KIND_DOT))
    {
        // interrupts stay ready for casts, crowd control and taunts have their own layers
        if (spell->Has(KIND_AOE_DAMAGE | KIND_GROUND | KIND_CC | KIND_TAUNT | KIND_INTERRUPT) || spell->Info->IsPositive() || spell->Cooldown > 30000
            || spell->Info->IsAutoRepeatRangedSpell())
            continue;
        if (spell->Has(KIND_DOT) && HasAuraFrom(target, spell, true))
            continue;
        if (!CanCast(spell, target))
            continue;
        float const rate = float(std::max(spell->Amount, 1)) / float(std::max<uint32>(spell->CastTime, 1500));
        if (!best || rate > bestRate)
        {
            best = spell;
            bestRate = rate;
        }
    }
    if (best)
        _brain.Propose(R_ROTATION + offset, best->Name.c_str(), target, [this, best, target]() { return Cast(best, target); }, "best damage known");

    // casters out of mana: the wand
    if (IsCasterClass() && me->GetPowerPct(POWER_MANA) < 25.0f && !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        if (BotSpell const* shoot = _spells.Find("Shoot"); shoot && CanCast(shoot, target))
            _brain.Propose(R_ROTATION + offset - 1, shoot->Name.c_str(), target, [this, shoot, target]() { return Cast(shoot, target); }, "low mana, wand");
    (void)ctx;
}
