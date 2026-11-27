/*
 * mod-class-tweaks (Priest): balance.
 *
 *   Smite and Holy Fire
 *       Damage x multiplier (Holy Fire's direct hit and its damage over time) for a real
 *       player priest not in Shadowform, against creatures only: players and anything a player
 *       controls are untouched, so PvP stays stock.
 *
 *   Holy AoE healing (Circle of Healing and Prayer of Healing, all ranks)
 *       For a real player priest with the Circle of Healing talent (41 points in Holy, so
 *       Discipline can't reach it), a spell script on both spells (bound in spell_script_names
 *       by this module's SQL) does two things on each healed target:
 *         - Spell power scaling: adds healing power x coefficient x (multiplier - 1), with the
 *           coefficient read from spell_bonus_data. Added before the core applies a critical
 *           heal, so crits scale it as usual.
 *         - Holy Concentration: a critical heal triggers the priest's Holy Concentration buff,
 *           as a critical Flash Heal or Greater Heal does. The buff for each talent rank and
 *           its proc chance are read from the talent's own spell data at startup.
 */

#include "ClassTweaks.h"
#include "Log.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <vector>

namespace
{
    using namespace ClassTweaks;

    constexpr uint32 SPELL_SMITE_RANK_1 = 585;
    constexpr uint32 SPELL_HOLY_FIRE_RANK_1 = 14914;
    constexpr uint32 SPELL_CIRCLE_OF_HEALING_RANK_1 = 34861;
    constexpr uint32 SPELL_HOLY_CONCENTRATION_RANK_1 = 34753;

    // Holy Concentration talent ranks, highest first: the talent spell, its buff and proc chance.
    struct HolyConcentrationRank
    {
        uint32 talent;
        uint32 buff;
        float chance;
    };
    std::vector<HolyConcentrationRank> holyConcentrationRanks;

    // The priest, if this damage should be multiplied.
    Player* SmiteHolyFirePriest(Unit* target, Unit* attacker, SpellInfo const* spellInfo)
    {
        if (config.smiteHolyFireMultiplier == 1.0f || spellInfo->SpellFamilyName != SPELLFAMILY_PRIEST)
            return nullptr;

        uint32 firstRank = spellInfo->GetFirstRankSpell()->Id;
        if (firstRank != SPELL_SMITE_RANK_1 && firstRank != SPELL_HOLY_FIRE_RANK_1)
            return nullptr;

        Player* priest = AsRealClass(attacker, CLASS_PRIEST);
        if (!priest || priest->GetShapeshiftForm() == FORM_SHADOW || !IsNonPlayerTarget(target))
            return nullptr;

        return priest;
    }
}

namespace ClassTweaks
{
    void PriestBalanceOnStartup()
    {
        holyConcentrationRanks.clear();

        for (SpellInfo const* rank = sSpellMgr->GetSpellInfo(SPELL_HOLY_CONCENTRATION_RANK_1); rank; rank = rank->GetNextRankSpell())
        {
            uint32 buff = 0;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (rank->Effects[i].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL && rank->Effects[i].TriggerSpell)
                    buff = rank->Effects[i].TriggerSpell;

            if (!buff || !sSpellMgr->GetSpellInfo(buff))
            {
                LOG_ERROR("module", "mod-class-tweaks: Holy Concentration rank {} has no buff to trigger; skipped.", rank->Id);
                continue;
            }

            float chance = float(rank->ProcChance);
            if (SpellProcEntry const* proc = sSpellMgr->GetSpellProcEntry(rank->Id))
                if (proc->Chance > 0.0f)
                    chance = proc->Chance;

            holyConcentrationRanks.insert(holyConcentrationRanks.begin(), { rank->Id, buff, chance });
        }

        LOG_INFO("module", "mod-class-tweaks: {} Holy Concentration ranks found.", holyConcentrationRanks.size());
    }

    void PriestOnSpellDamage(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
    {
        if (damage > 0 && SmiteHolyFirePriest(target, attacker, spellInfo))
            damage = int32(damage * config.smiteHolyFireMultiplier);
    }

    void PriestOnPeriodicDamage(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo)
    {
        if (damage && SmiteHolyFirePriest(target, attacker, spellInfo))
            damage = uint32(damage * config.smiteHolyFireMultiplier);
    }
}

// Circle of Healing and Prayer of Healing (bound to -34861 and -596 in spell_script_names).
class spell_class_tweaks_holy_aoe : public SpellScript
{
    PrepareSpellScript(spell_class_tweaks_holy_aoe);

    bool IsCrit(Unit* target)
    {
        for (TargetInfo const& info : *GetSpell()->GetUniqueTargetInfo())
            if (info.targetGUID == target->GetGUID())
                return info.crit;

        return false;
    }

    // Runs after the heal is calculated and before the core applies a critical heal.
    void HandleOnHit()
    {
        if (config.holyAoESpellPowerMultiplier == 1.0f && !config.holyConcentrationAoE)
            return;

        Unit* target = GetHitUnit();
        Player* priest = AsRealClass(GetCaster(), CLASS_PRIEST);
        if (!target || !priest || !priest->HasTalent(SPELL_CIRCLE_OF_HEALING_RANK_1, priest->GetActiveSpec()))
            return;

        int32 heal = GetHitHeal();
        if (heal <= 0)
            return;

        if (config.holyAoESpellPowerMultiplier != 1.0f)
        {
            SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(GetSpellInfo()->Id);
            if (bonus && bonus->direct_damage > 0.0f)
            {
                int32 healingPower = priest->SpellBaseHealingBonusDone(GetSpellInfo()->GetSchoolMask());
                int32 extra = int32(healingPower * bonus->direct_damage * (config.holyAoESpellPowerMultiplier - 1.0f));
                if (extra > 0)
                    SetHitHeal(heal + extra);
            }
        }

        if (config.holyConcentrationAoE && IsCrit(target))
        {
            for (HolyConcentrationRank const& rank : holyConcentrationRanks)
            {
                if (!priest->HasTalent(rank.talent, priest->GetActiveSpec()))
                    continue;

                if (roll_chance_f(rank.chance))
                    priest->CastSpell(priest, rank.buff, true);
                break;
            }
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_class_tweaks_holy_aoe::HandleOnHit);
    }
};

namespace ClassTweaks
{
    void AddPriestSpellScripts()
    {
        RegisterSpellScript(spell_class_tweaks_holy_aoe);
    }
}
