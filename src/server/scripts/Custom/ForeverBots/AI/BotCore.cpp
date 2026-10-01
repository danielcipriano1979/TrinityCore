/*
 * Forever bots: spell book (classification from SpellInfo), party blackboard and the relevance brain.
 */

#include "BotAI.h"
#include "Log.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>

namespace ForeverBots
{
    char const* BotStateName(BotState state)
    {
        switch (state)
        {
            case BotState::Combat: return "COMBAT";
            case BotState::Rest:   return "REST";
            case BotState::Dead:   return "DEAD";
            default:               return "IDLE";
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Spell book
    // ---------------------------------------------------------------------------------------------------------------------
    namespace
    {
        bool IsEnemyCheck(SpellEffectInfo const& effect)
        {
            return effect.TargetA.GetCheckType() == TARGET_CHECK_ENEMY;
        }

        bool IsCasterOnly(SpellInfo const* info)
        {
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect() && effect.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                    return false;
            return true;
        }

        int32 PeriodicTotal(SpellEffectInfo const& effect, int32 value, int32 duration)
        {
            if (!effect.ApplyAuraPeriod || duration <= 0)
                return value;
            return value * std::max<int32>(1, duration / int32(effect.ApplyAuraPeriod));
        }

        BotSpell Classify(Player* me, SpellInfo const* info, std::string const& name)
        {
            BotSpell spell;
            spell.Info = info;
            spell.Name = name;
            spell.CastTime = info->CalcCastTime();
            spell.Cooldown = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
            spell.Range = info->GetMaxRange(info->IsPositive(), me);

            bool const area = info->IsAffectingArea();
            int32 const duration = info->GetMaxDuration();
            bool const casterOnly = IsCasterOnly(info);

            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (!effect.IsEffect())
                    continue;

                int32 const value = int32(effect.CalcValue(me));
                bool const enemy = IsEnemyCheck(effect);
                if (effect.TargetA.GetTarget() == TARGET_DEST_DEST || effect.TargetB.GetTarget() == TARGET_DEST_DEST)
                    spell.Kinds |= KIND_GROUND;

                switch (effect.Effect)
                {
                    case SPELL_EFFECT_HEAL:
                    case SPELL_EFFECT_HEAL_PCT:
                        spell.Kinds |= area ? KIND_AOE_HEAL : KIND_DIRECT_HEAL;
                        spell.Amount += effect.Effect == SPELL_EFFECT_HEAL_PCT ? int32(me->CountPctFromMaxHealth(value)) : value;
                        break;
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                    case SPELL_EFFECT_HEALTH_LEECH:
                        spell.Kinds |= area ? KIND_AOE_DAMAGE : KIND_DAMAGE;
                        spell.Amount += value;
                        break;
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                        spell.Kinds |= KIND_MELEE | (area ? KIND_AOE_DAMAGE : KIND_DAMAGE);
                        spell.Amount += std::max(value, 1);
                        break;
                    case SPELL_EFFECT_INTERRUPT_CAST:
                        spell.Kinds |= KIND_INTERRUPT;
                        break;
                    case SPELL_EFFECT_DISPEL:
                        spell.DispelMask |= SpellInfo::GetDispelMask(DispelType(effect.MiscValue));
                        if (effect.TargetA.GetCheckType() != TARGET_CHECK_ENEMY)
                            spell.Kinds |= KIND_DISPEL;
                        if (effect.TargetA.GetCheckType() == TARGET_CHECK_ENEMY || effect.TargetA.GetCheckType() == TARGET_CHECK_DEFAULT)
                            spell.Kinds |= KIND_PURGE;
                        break;
                    case SPELL_EFFECT_ATTACK_ME:
                        spell.Kinds |= KIND_TAUNT;
                        break;
                    case SPELL_EFFECT_RESURRECT:
                    case SPELL_EFFECT_RESURRECT_WITH_AURA:
                        spell.Kinds |= KIND_RESURRECT;
                        break;
                    case SPELL_EFFECT_MODIFY_THREAT_PERCENT:
                        if (value < 0)
                            spell.Kinds |= KIND_THREAT_DROP;
                        break;
                    case SPELL_EFFECT_APPLY_AURA:
                    case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                    case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    {
                        switch (effect.ApplyAuraName)
                        {
                            case SPELL_AURA_PERIODIC_HEAL:
                                spell.Kinds |= area ? KIND_AOE_HEAL : KIND_HOT;
                                spell.Amount += PeriodicTotal(effect, value, duration);
                                break;
                            case SPELL_AURA_SCHOOL_ABSORB:
                                if (!enemy)
                                    spell.Kinds |= casterOnly && spell.Cooldown >= 20000 ? KIND_DEFENSIVE : KIND_SHIELD;
                                spell.Amount += value;
                                break;
                            case SPELL_AURA_PERIODIC_DAMAGE:
                            case SPELL_AURA_PERIODIC_LEECH:
                            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                                spell.Kinds |= (area || effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA) ? KIND_AOE_DAMAGE : KIND_DOT;
                                spell.Amount += PeriodicTotal(effect, value, duration);
                                break;
                            case SPELL_AURA_MOD_TAUNT:
                                spell.Kinds |= KIND_TAUNT;
                                break;
                            case SPELL_AURA_MOD_STUN:
                            case SPELL_AURA_MOD_FEAR:
                            case SPELL_AURA_MOD_CONFUSE:
                            case SPELL_AURA_MOD_CHARM:
                            case SPELL_AURA_TRANSFORM:
                            case SPELL_AURA_SCHOOL_IMMUNITY:
                                if (enemy)
                                    spell.Kinds |= KIND_CC;
                                else if (effect.ApplyAuraName == SPELL_AURA_SCHOOL_IMMUNITY && casterOnly)
                                    spell.Kinds |= KIND_DEFENSIVE;
                                break;
                            case SPELL_AURA_MOD_ROOT:
                            case SPELL_AURA_MOD_DECREASE_SPEED:
                                if (enemy)
                                    spell.Kinds |= KIND_SNARE;
                                break;
                            case SPELL_AURA_MOD_TOTAL_THREAT:
                            case SPELL_AURA_FEIGN_DEATH:
                                if (value < 0 || effect.ApplyAuraName == SPELL_AURA_FEIGN_DEATH)
                                    spell.Kinds |= KIND_THREAT_DROP;
                                break;
                            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                                if (!enemy && value < 0 && spell.Cooldown >= 20000)
                                    spell.Kinds |= KIND_DEFENSIVE;
                                break;
                            case SPELL_AURA_MOD_SHAPESHIFT:
                            case SPELL_AURA_MOUNTED:
                            case SPELL_AURA_TRACK_CREATURES:
                            case SPELL_AURA_TRACK_RESOURCES:
                                break;     // stances, forms, mounts and tracking belong to the class layer
                            default:
                                // long positive auras: buffs (self only, or for the party)
                                if (!enemy && info->IsPositive() && (duration < 0 || duration >= 120000) && !spell.Cooldown)
                                    spell.Kinds |= casterOnly ? KIND_SELF_BUFF : KIND_BUFF;
                                break;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            // stances, forms, aspects, seals, armors, auras, elemental shields: exclusive, the class layer picks one
            if (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL)
                spell.Kinds &= ~(KIND_BUFF | KIND_SELF_BUFF);
            return spell;
        }
    }

    bool BotSpellBook::NeedsRebuild(Player const* me) const
    {
        return _level != me->GetLevel() || _spellCount != me->GetSpellMap().size();
    }

    void BotSpellBook::Build(Player* me)
    {
        _byName.clear();
        _ordered.clear();

        for (auto const& [spellId, known] : me->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!info || !info->SpellName || info->IsPassive() || info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
                continue;
            if (!sForeverBotMgr->IsClassicSpell(me->GetClass(), info))
                continue;       // later expansions' class spells from the client data
            char const* name = (*info->SpellName)[LOCALE_enUS];
            if (!name || !*name)
                continue;

            // the best rank of each name (the Classic data has several ranks)
            auto itr = _byName.find(name);
            if (itr != _byName.end() && (itr->second.Info->SpellLevel > info->SpellLevel
                || (itr->second.Info->SpellLevel == info->SpellLevel && itr->second.Info->Id > info->Id)))
                continue;
            _byName[name] = Classify(me, info, name);
        }

        for (auto const& [name, spell] : _byName)
            _ordered.push_back(&spell);
        _level = me->GetLevel();
        _spellCount = me->GetSpellMap().size();
    }

    BotSpell const* BotSpellBook::Find(std::string_view name) const
    {
        auto itr = _byName.find(std::string(name));
        return itr != _byName.end() ? &itr->second : nullptr;
    }

    std::vector<BotSpell const*> BotSpellBook::Of(uint32 kinds) const
    {
        std::vector<BotSpell const*> result;
        for (BotSpell const* spell : _ordered)
            if (spell->Kinds & kinds)
                result.push_back(spell);
        return result;
    }

    std::string BotSpellBook::Describe() const
    {
        std::string text;
        for (BotSpell const* spell : _ordered)
            if (spell->Kinds)
                text += Trinity::StringFormat("{}({}:{:#x}) ", spell->Name, spell->Info->Id, spell->Kinds);
        return text;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Party blackboard
    // ---------------------------------------------------------------------------------------------------------------------
    int32 PartyBlackboard::IncomingHeal(ObjectGuid target, uint32 now) const
    {
        auto itr = Heals.find(target);
        if (itr == Heals.end())
            return 0;
        int32 total = 0;
        for (Heal const& heal : itr->second)
            if (heal.Expires > now)
                total += heal.Amount;
        return total;
    }

    void PartyBlackboard::AddHeal(ObjectGuid target, ObjectGuid healer, int32 amount, uint32 expires)
    {
        Heals[target].push_back({ healer, amount, expires });
    }

    bool PartyBlackboard::IsClaimedByOther(std::unordered_map<ObjectGuid, Claim> const& claims, ObjectGuid enemy, ObjectGuid bot, uint32 now) const
    {
        auto itr = claims.find(enemy);
        return itr != claims.end() && itr->second.Expires > now && itr->second.Bot != bot;
    }

    bool PartyBlackboard::TryClaim(std::unordered_map<ObjectGuid, Claim>& claims, ObjectGuid enemy, ObjectGuid bot, uint32 now, uint32 duration)
    {
        if (IsClaimedByOther(claims, enemy, bot, now))
            return false;
        claims[enemy] = { bot, now + duration };
        return true;
    }

    void PartyBlackboard::Prune(uint32 now)
    {
        std::erase_if(Interrupts, [now](auto const& entry) { return entry.second.Expires <= now; });
        std::erase_if(CrowdControl, [now](auto const& entry) { return entry.second.Expires <= now; });
        for (auto itr = Heals.begin(); itr != Heals.end(); )
        {
            std::erase_if(itr->second, [now](Heal const& heal) { return heal.Expires <= now; });
            itr = itr->second.empty() ? Heals.erase(itr) : std::next(itr);
        }
        if (EngagedAt.size() > 64)
            EngagedAt.clear();
        if (FocusExpires && FocusExpires <= now)
        {
            FocusTarget.Clear();
            FocusExpires = 0;
        }
    }

    PartyRegistry& PartyRegistry::Instance()
    {
        static PartyRegistry registry;
        return registry;
    }

    PartyBlackboard& PartyRegistry::For(ObjectGuid key, uint32 now)
    {
        std::lock_guard<std::mutex> lock(_lock);
        PartyBlackboard& party = _parties[key];
        party.LastUse = now;
        if (now - _lastCleanup > 60000)
        {
            _lastCleanup = now;
            std::erase_if(_parties, [now, &party](auto const& entry) { return &entry.second != &party && now - entry.second.LastUse > 300000; });
        }
        return party;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Brain
    // ---------------------------------------------------------------------------------------------------------------------
    void BotBrain::Propose(int32 relevance, char const* name, Unit* target, std::function<bool()> run, std::string reason)
    {
        Candidate& candidate = _candidates.emplace_back();
        candidate.Relevance = relevance;
        candidate.Name = name;
        candidate.Target = target;
        candidate.Run = std::move(run);
        candidate.Reason = std::move(reason);
    }

    Candidate const* BotBrain::Execute()
    {
        // most relevant first; a candidate that can't run now doesn't use up the tick, the next one is tried
        std::stable_sort(_candidates.begin(), _candidates.end(), [](Candidate const& a, Candidate const& b) { return a.Relevance > b.Relevance; });
        for (Candidate& candidate : _candidates)
        {
            if (candidate.Run && candidate.Run())
            {
                _done = std::move(candidate);
                _candidates.clear();
                return &_done;
            }
        }
        _candidates.clear();
        return nullptr;
    }

    BotRole BotContext::RoleOf(Player const* member) const
    {
        if (member == Me && Data)
            return Data->Role;
        if (ForeverBot const* bot = sForeverBotMgr->GetBot(member->GetGUID()))
            return bot->Role;
        if (Party && member->GetGUID() == Party->Tank)
            return BotRole::Tank;
        // real players: a pure healer class counts as healer only if it isn't the tank
        return member->GetClass() == CLASS_PRIEST ? BotRole::Heal : BotRole::Dps;
    }
}
