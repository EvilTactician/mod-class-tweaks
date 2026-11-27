/*
 * mod-class-tweaks
 *
 * Shared config, per-player state and helpers. Every feature only affects real players:
 * playerbots are skipped before any work is done.
 */

#ifndef MOD_CLASS_TWEAKS_H
#define MOD_CLASS_TWEAKS_H

#include "DataMap.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "WorldSession.h"
#include <chrono>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

class Item;
class Spell;
class SpellInfo;
class Unit;
class WorldPacket;

#define CLASS_TWEAKS_VERSION "1.0.0"

namespace ClassTweaks
{
    struct Config
    {
        // Rogue: combo points follow the rogue
        bool comboEnabled = true;
        uint32 comboKeepAfterKill = 20000;

        // Rogue: quick lockpick
        bool lockpickRightClick = true;
        bool lockpickOpenAfterUnlock = true;

        // Rogue: poisons
        uint32 poisonDurationMs = 0;          // 0 = stock (1 hour)
        bool poisonAutoApply = true;          // feature on; characters start with it on
        bool poisonAutoApplyOutside = false;  // characters start with "outside instances" off
        bool poisonAllowOutside = true;       // characters may turn "outside instances" on
        bool poisonNotify = true;
        float poisonAPMultiplier = 1.0f;      // 1.0 = stock

        // Rogue: Find Weakness
        bool findWeaknessBypass = true;
        bool findWeaknessNotify = true;

        // Rogue: Rupture
        float ruptureMultiplier = 1.0f;       // 1.0 = stock

        // Priest
        float smiteHolyFireMultiplier = 1.0f; // 1.0 = stock
        bool holyConcentrationAoE = true;
        float holyAoESpellPowerMultiplier = 1.0f;
    };

    extern Config config;

    // Everything the module keeps per player, in one DataMap entry.
    struct PlayerState : public DataMap::Base
    {
        // Rogue: combo points
        uint8 lastPoints = 0;                 // points and target as of the last update
        ObjectGuid lastTarget;
        uint8 savedPoints = 0;                // points from a target that died, waiting for the next one
        std::chrono::milliseconds savedUntil{0};
        std::chrono::milliseconds finisherUntil{0};

        // Rogue: the box a right-click Pick Lock is under way on
        ObjectGuid pendingBox;

        // Rogue: a poison that ran out on a weapon, re-applied on the next update
        struct PendingPoison
        {
            ObjectGuid item;
            uint32 enchantId = 0;
        };
        PendingPoison pendingPoison[2];       // 0 = main hand, 1 = off hand
        bool hasPendingPoison = false;

        // Rogue: last target the Find Weakness message was shown for
        ObjectGuid findWeaknessNotified;
    };

    PlayerState* GetState(Player* player);

    // Bot sessions: IsHeadless() on current playerbots cores, IsBot() on older ones. If a core
    // update removes both, the build fails here instead of silently treating bots as players.
    template <typename> inline constexpr bool AlwaysFalse = false;

    template <typename S, typename = void>
    struct HasIsHeadless : std::false_type { };
    template <typename S>
    struct HasIsHeadless<S, std::void_t<decltype(std::declval<S const&>().IsHeadless())>> : std::true_type { };

    template <typename S, typename = void>
    struct HasIsBot : std::false_type { };
    template <typename S>
    struct HasIsBot<S, std::void_t<decltype(std::declval<S const&>().IsBot())>> : std::true_type { };

    template <typename S>
    inline bool IsBotSession(S const* session)
    {
        if constexpr (HasIsHeadless<S>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<S>::value)
            return session->IsBot();
        else
        {
            static_assert(AlwaysFalse<S>, "mod-class-tweaks: no playerbot session check found on WorldSession");
            return false;
        }
    }

    inline bool IsRealPlayer(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return session && !IsBotSession(session);
    }

    // Cheapest checks first: class, then the bot check.
    inline bool IsRealClass(Player const* player, uint8 playerClass)
    {
        return player->getClass() == playerClass && IsRealPlayer(player);
    }

    // The unit as a real player of the given class, or nullptr.
    inline Player* AsRealClass(Unit* unit, uint8 playerClass)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        return (player && IsRealClass(player, playerClass)) ? player : nullptr;
    }

    // A creature that no player controls (not a player, pet, guardian or charmed unit).
    inline bool IsNonPlayerTarget(Unit const* target)
    {
        return target && !target->GetCharmerOrOwnerPlayerOrPlayerItself();
    }

    // Rogue: combo points (RogueComboPoints.cpp)
    void ComboOnSelect(Player* player, ObjectGuid guid);
    void ComboOnSpellCast(Player* player, Spell* spell);
    void ComboOnUpdate(Player* player, PlayerState* state);

    // Rogue: quick lockpick (RogueQuickLockpick.cpp)
    bool LockpickOnOpenItem(Player* player, WorldPacket const& packet);  // false = packet handled
    void LockpickOnSpellCast(Player* player, Spell* spell, SpellInfo const* spellInfo);
    void LockpickOnSpellCastCancel(Player* player, Spell* spell);

    // Rogue: poisons (RoguePoisons.cpp)
    void PoisonBuildTables();
    bool IsRoguePoisonProcSpell(uint32 spellId);
    std::unordered_set<uint32> const& RoguePoisonProcSpells();
    void PoisonOnSpellCast(Player* player, Spell* spell, SpellInfo const* spellInfo);
    void PoisonOnEnchantRemoved(Player* player, Item* item);
    void PoisonOnUpdate(Player* player, PlayerState* state);
    bool PoisonAutoApplyOn(Player* player);
    bool PoisonAutoApplyOutsideOn(Player* player);
    void PoisonSetAutoApply(Player* player, bool on);
    void PoisonSetAutoApplyOutside(Player* player, bool on);
    void PoisonSendState(Player* player);
    bool PoisonOnAddonMessage(Player* player, std::string const& msg);  // false = not ours

    // Rogue: balance (RogueBalance.cpp)
    void RogueBalanceOnStartup();
    void RogueOnSpellCheckCast(Spell* spell, SpellInfo const* spellInfo, SpellCastResult& res);
    void RogueOnSpellDamage(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo);
    void RogueOnPeriodicDamage(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo);

    // Priest (PriestBalance.cpp)
    void PriestBalanceOnStartup();
    void PriestOnSpellDamage(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo);
    void PriestOnPeriodicDamage(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo);
    void AddPriestSpellScripts();
}

#endif
