/*
 * mod-class-tweaks (all classes): talent reset without a trainer.
 *
 * An addon (my DragonUI fork's talent window) can ask the realm to unlearn the talents of the
 * active spec from anywhere. The core's own Player::resetTalents does the work, exactly as at a
 * class trainer: the realm's reset cost (NoResetTalentsCost in worldserver.conf), the rising
 * cost and its monthly decay, the gold, the achievements, the pet being dismissed.
 *
 * Not in combat, while dead, or in a battleground or arena. The cost is checked here before the
 * core is called: Player::resetTalents clears its used-talent count before its own gold check,
 * so a reset it refuses for gold would leave that count wrong.
 *
 * Addon messages, prefix "ClassTweaks", whispered to oneself:
 *   client -> server   TALENTS HELLO 1        is the feature there?
 *                      TALENTS COST           what a reset costs right now
 *                      TALENTS RESET <copper> reset; <copper> is the cost the player agreed to
 *   server -> client   TALENTS 1              answer to HELLO (protocol 1)
 *                      TALENTS COST <copper>
 *                      TALENTS DONE
 *                      TALENTS ERR <COMBAT|DEAD|BATTLEGROUND|NOTALENTS|MONEY>
 *                      TALENTS ERR COST <copper>   the cost went up since the player agreed
 * Feature off: no answer, so the addon shows nothing.
 */

#include "ClassTweaks.h"
#include "Map.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "World.h"

namespace
{
    using namespace ClassTweaks;

    constexpr uint32 TALENT_PROTOCOL = 1;
    constexpr uint32 SPELL_UNTALENT_VISUAL = 14867;  // what a trainer casts after a reset

    // What the core will charge for the next reset.
    uint32 ResetCost(Player* player)
    {
        return sWorld->getBoolConfig(CONFIG_NO_RESET_TALENT_COST) ? 0 : player->resetTalentsCost();
    }

    // Why a reset can't happen right now, or nullptr.
    char const* Blocked(Player* player)
    {
        if (player->IsInCombat())
            return "COMBAT";
        if (!player->IsAlive())
            return "DEAD";
        if (Map* map = player->GetMap(); map && map->IsBattlegroundOrArena())
            return "BATTLEGROUND";
        if (player->CalculateTalentsPoints() <= player->GetFreeTalentPoints())
            return "NOTALENTS";
        return nullptr;
    }

    void Reply(Player* player, std::string const& text)
    {
        SendAddonMessage(player, "TALENTS " + text);
    }

    void Reset(Player* player, std::string_view agreedText)
    {
        if (char const* reason = Blocked(player))
        {
            Reply(player, Acore::StringFormat("ERR {}", reason));
            return;
        }

        uint32 const cost = ResetCost(player);
        if (cost > Acore::StringTo<uint32>(agreedText).value_or(0))
        {
            Reply(player, Acore::StringFormat("ERR COST {}", cost));
            return;
        }

        if (!player->HasEnoughMoney(cost))
        {
            Reply(player, "ERR MONEY");
            return;
        }

        // As at a trainer: Feign Death ends.
        if (player->HasUnitState(UNIT_STATE_DIED))
            player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        if (!player->resetTalents())
        {
            Reply(player, "ERR NOTALENTS");
            return;
        }

        player->SendTalentsInfoData(false);
        player->CastSpell(player, SPELL_UNTALENT_VISUAL, true);
        Reply(player, "DONE");
    }
}

namespace ClassTweaks
{
    // An addon message the player whispered to themselves. Returns false if it isn't a TALENTS one.
    bool TalentResetOnAddonMessage(Player* player, std::string const& msg)
    {
        static std::string const prefix = ADDON_PREFIX + "TALENTS ";
        if (msg.compare(0, prefix.size(), prefix) != 0)
            return false;

        if (!config.talentResetEnabled || !IsRealPlayer(player))
            return true;

        std::string_view const text = std::string_view(msg).substr(prefix.size());

        if (text.rfind("HELLO", 0) == 0)
            Reply(player, Acore::StringFormat("{}", TALENT_PROTOCOL));
        else if (text == "COST")
            Reply(player, Acore::StringFormat("COST {}", ResetCost(player)));
        else if (text.rfind("RESET ", 0) == 0)
            Reset(player, text.substr(6));

        return true;
    }
}
