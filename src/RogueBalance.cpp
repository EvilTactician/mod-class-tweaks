/*
 * mod-class-tweaks (Rogue): balance.
 *
 *   Find Weakness poison bypass
 *       A real player with Find Weakness rank 3 (an Assassination talent) has their poisons and
 *       Envenom go through a creature's own Nature immunity (creature_immunities). Everyone
 *       else, including playerbots, gets "Immune" as in stock.
 *
 *       How: at startup the poison proc spells and Envenom get the core's "ignore school
 *       immunity" attribute (SPELL_ATTR2_NO_SCHOOL_IMMUNITIES), which every school immunity
 *       check in the core honours. Then, before each of those casts on a Nature-immune target,
 *       OnSpellCheckCast lets it through only for a qualifying rogue against a creature whose
 *       immunity comes from its template, and fails it with SPELL_FAILED_IMMUNE otherwise
 *       (other rogues, bots, players and their pets, temporary immunity auras such as Divine
 *       Shield or a boss immunity phase). A failed Envenom keeps its combo points.
 *       Only school immunity is bypassed: mechanic immunities (e.g. to Crippling's snare) stay.
 *       Spell data can only be changed safely before the world runs, so switching this on
 *       needs a restart; switching it off works with .reload config.
 *
 *   Poison attack power scaling
 *       The attack power part of Instant, Deadly (per stack) and Wound Poison damage is
 *       multiplied: extra = AP x coefficient x (multiplier - 1), with the coefficient read from
 *       spell_bonus_data. Damage only; Wound Poison's healing reduction is untouched.
 *
 *   Rupture
 *       Rupture damage per tick x multiplier.
 */

#include "ClassTweaks.h"
#include "Chat.h"
#include "Log.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <limits>
#include <unordered_set>

namespace
{
    using namespace ClassTweaks;

    constexpr uint32 SPELL_FIND_WEAKNESS_RANK_3 = 31236;
    constexpr uint32 SPELL_ENVENOM_RANK_1 = 32645;
    constexpr uint32 SPELL_RUPTURE_RANK_1 = 1943;
    constexpr uint32 SPELLFAMILYFLAG0_RUPTURE = 0x00100000;

    // The core loads creature_immunities with this as the "spell" that gives the immunity.
    constexpr uint32 TEMPLATE_IMMUNITY = std::numeric_limits<uint32>::max();

    // Spells this module gave the "ignore school immunity" attribute at startup.
    std::unordered_set<uint32> bypassSpells;

    enum class SchoolImmunity
    {
        None,          // not immune to all of these schools
        TemplateOnly,  // immune, only through the creature's own data
        Other          // immune through an aura (Divine Shield, Ice Block, a boss phase, ...)
    };

    SchoolImmunity GetSchoolImmunity(Unit const* target, uint32 schoolMask)
    {
        uint32 covered = 0;
        bool fromAura = false;

        for (uint32 type : { uint32(IMMUNITY_SCHOOL), uint32(IMMUNITY_DAMAGE) })
        {
            for (auto const& [mask, spellId] : target->m_spellImmune[type])
            {
                if (!(mask & schoolMask))
                    continue;

                covered |= mask;
                if (spellId != TEMPLATE_IMMUNITY)
                    fromAura = true;
            }
        }

        if ((covered & schoolMask) != schoolMask)
            return SchoolImmunity::None;

        return fromAura ? SchoolImmunity::Other : SchoolImmunity::TemplateOnly;
    }

    bool IsBypassSpell(SpellInfo const* spellInfo)
    {
        return spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE && bypassSpells.count(spellInfo->Id);
    }

    bool HasFindWeakness(Player* rogue)
    {
        return rogue->HasTalent(SPELL_FIND_WEAKNESS_RANK_3, rogue->GetActiveSpec());
    }

    void NotifyBypass(Player* rogue, Unit* target)
    {
        if (!config.findWeaknessNotify)
            return;

        PlayerState* state = GetState(rogue);
        if (state->findWeaknessNotified == target->GetGUID())
            return;

        state->findWeaknessNotified = target->GetGUID();
        ChatHandler(rogue->GetSession()).PSendSysMessage("Find Weakness: your poisons pierce {}'s Nature immunity.", target->GetName());
    }

    // Extra damage from the poison's attack power part, or 0.
    int32 PoisonAPExtra(Player* rogue, SpellInfo const* spellInfo, bool periodic, uint8 stacks)
    {
        if (!IsRoguePoisonProcSpell(spellInfo->Id))
            return 0;

        SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spellInfo->Id);
        float coefficient = bonus ? (periodic ? bonus->ap_dot_bonus : bonus->ap_bonus) : 0.0f;
        if (coefficient <= 0.0f)
            return 0;

        float ap = rogue->GetTotalAttackPowerValue(BASE_ATTACK);
        return int32(ap * coefficient * (config.poisonAPMultiplier - 1.0f) * stacks);
    }

    bool IsRupture(SpellInfo const* spellInfo)
    {
        return (spellInfo->SpellFamilyFlags[0] & SPELLFAMILYFLAG0_RUPTURE) && spellInfo->GetFirstRankSpell()->Id == SPELL_RUPTURE_RANK_1;
    }
}

namespace ClassTweaks
{
    // Runs once at startup, after the poison tables are built and before the world runs.
    void RogueBalanceOnStartup()
    {
        bypassSpells.clear();

        if (!config.findWeaknessBypass)
        {
            LOG_INFO("module", "mod-class-tweaks: Find Weakness poison bypass is off (switching it on needs a restart).");
            return;
        }

        std::unordered_set<uint32> spells = RoguePoisonProcSpells();
        for (SpellInfo const* envenom = sSpellMgr->GetSpellInfo(SPELL_ENVENOM_RANK_1); envenom; envenom = envenom->GetNextRankSpell())
            spells.insert(envenom->Id);

        for (uint32 spellId : spells)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);

            // Spells that already ignore school immunity for everyone are left alone.
            if (!spellInfo || spellInfo->HasAttribute(SPELL_ATTR2_NO_SCHOOL_IMMUNITIES))
                continue;

            const_cast<SpellInfo*>(spellInfo)->AttributesEx2 |= SPELL_ATTR2_NO_SCHOOL_IMMUNITIES;
            bypassSpells.insert(spellId);
        }

        LOG_INFO("module", "mod-class-tweaks: Find Weakness poison bypass ready ({} spells).", bypassSpells.size());
    }

    // Before every cast (all spells): enforce immunity for the spells we let through it.
    void RogueOnSpellCheckCast(Spell* spell, SpellInfo const* spellInfo, SpellCastResult& res)
    {
        if (res != SPELL_CAST_OK || !IsBypassSpell(spellInfo))
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target)
            return;

        SchoolImmunity immunity = GetSchoolImmunity(target, spellInfo->GetSchoolMask());
        if (immunity == SchoolImmunity::None)
            return;

        if (immunity == SchoolImmunity::TemplateOnly && config.findWeaknessBypass && IsNonPlayerTarget(target))
        {
            Unit* caster = spell->GetCaster() ? spell->GetCaster()->ToUnit() : nullptr;
            if (Player* rogue = AsRealClass(caster, CLASS_ROGUE))
            {
                if (HasFindWeakness(rogue))
                {
                    NotifyBypass(rogue, target);
                    return;
                }
            }
        }

        res = SPELL_FAILED_IMMUNE;
    }

    void RogueOnSpellDamage(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* spellInfo)
    {
        if (config.poisonAPMultiplier == 1.0f || damage <= 0)
            return;

        if (Player* rogue = AsRealClass(attacker, CLASS_ROGUE))
            damage += PoisonAPExtra(rogue, spellInfo, false, 1);
    }

    void RogueOnPeriodicDamage(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo)
    {
        // A poison that got through a creature's immunity, on a target that has since gained an
        // immunity aura: that aura wins.
        if (!bypassSpells.empty() && IsBypassSpell(spellInfo) && GetSchoolImmunity(target, spellInfo->GetSchoolMask()) == SchoolImmunity::Other)
        {
            damage = 0;
            return;
        }

        Player* rogue = AsRealClass(attacker, CLASS_ROGUE);
        if (!rogue)
            return;

        if (config.ruptureMultiplier != 1.0f && IsRupture(spellInfo))
        {
            damage = uint32(damage * config.ruptureMultiplier);
            return;
        }

        if (config.poisonAPMultiplier != 1.0f && damage)
        {
            Aura const* aura = target->GetAura(spellInfo->Id, rogue->GetGUID());
            uint8 stacks = aura ? std::max<uint8>(aura->GetStackAmount(), 1) : 1;
            int32 extra = PoisonAPExtra(rogue, spellInfo, true, stacks);
            if (extra > 0)
                damage += uint32(extra);
        }
    }
}
