/*
 * mod-class-tweaks (Rogue): poisons.
 *
 *   Duration    Poisons applied by a real player last Poison.DurationMinutes instead of 1 hour.
 *               The client shows the real time left; no client patch needed.
 *   Auto-apply  A poison that runs out on the main or off hand is put back on that same weapon
 *               right away, using one of that exact poison (same rank) from the bags. Inside
 *               dungeons and raids, and optionally everywhere else. None left in the bags,
 *               another poison or weapon there by then: nothing happens.
 *
 * Each character can switch auto-apply (and "outside instances") on or off with a chat command
 * or an addon; the choice is kept in the core's character settings. The conf file gives the
 * starting values.
 *
 * Which items are poisons is worked out once at startup from the item and enchant data: a
 * consumable whose spell puts a temporary enchant on a weapon, where that enchant procs a rogue
 * spell (Instant, Deadly, Wound, Crippling, Mind-numbing, Anesthetic). No hardcoded item IDs.
 *
 * Expiry is caught by the core's OnPlayerCanApplyEnchantment hook, which UpdateEnchantTime calls
 * (apply = false, apply_dur = false) while the enchant is still on the item, so the module knows
 * exactly which poison was on which weapon. The re-apply waits for the player's next update,
 * because the core is still iterating its enchant timer list at that point.
 */

#include "ClassTweaks.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace ClassTweaks;

    constexpr uint32 STOCK_POISON_DURATION_MS = 3600 * IN_MILLISECONDS;

    // Per-character settings, stored by the core (character_settings table).
    // 0 = not chosen (use the conf value), 1 = off, 2 = on.
    std::string const SETTINGS_SOURCE = "mod-class-tweaks";
    constexpr uint32 SETTING_AUTO_APPLY = 0;
    constexpr uint32 SETTING_AUTO_APPLY_OUTSIDE = 1;
    constexpr uint32 SETTING_OFF = 1;
    constexpr uint32 SETTING_ON = 2;

    // Addon messages: "ClassTweaks\t<text>", whispered to oneself.
    std::string const ADDON_PREFIX = "ClassTweaks\t";
    constexpr uint32 ADDON_PROTOCOL = 1;

    // Poison enchant -> the poison items that apply it (normally one).
    std::unordered_map<uint32, std::vector<uint32>> poisonItemsByEnchant;

    // Poison application spell -> its enchant.
    std::unordered_map<uint32, uint32> poisonEnchantBySpell;

    // The spells poisons cast when they proc on a hit.
    std::unordered_set<uint32> poisonProcSpells;

    // The rogue proc spell of a poison enchant, or 0.
    uint32 RoguePoisonProcSpell(uint32 enchantId)
    {
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant)
            return 0;

        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            if (enchant->type[i] != ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL || !enchant->spellid[i])
                continue;

            if (SpellInfo const* proc = sSpellMgr->GetSpellInfo(enchant->spellid[i]))
                if (proc->SpellFamilyName == SPELLFAMILY_ROGUE)
                    return proc->Id;
        }

        return 0;
    }

    uint32 PoisonDuration()
    {
        return config.poisonDurationMs ? config.poisonDurationMs : STOCK_POISON_DURATION_MS;
    }

    // What the core's temporary enchant effect does, with our duration.
    void SetPoison(Player* player, Item* item, uint32 enchantId, uint32 durationMs)
    {
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant)
            return;

        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, durationMs, enchant->charges, player->GetGUID());
        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, true);
    }

    char const* ItemName(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1.c_str() : "poison";
    }

    bool InInstance(Player* player)
    {
        Map* map = player->GetMap();
        return map && map->IsDungeon();
    }

    // Whether auto-apply is on for this player where they are now.
    bool AutoApplyHere(Player* player)
    {
        if (!PoisonAutoApplyOn(player))
            return false;

        return InInstance(player) || PoisonAutoApplyOutsideOn(player);
    }

    uint32 GetSetting(Player* player, uint32 index)
    {
        return player->GetPlayerSetting(SETTINGS_SOURCE, index).value;
    }

    void ReapplyPoison(Player* player, uint8 weaponIndex, PlayerState::PendingPoison const& pending)
    {
        uint8 const slot = weaponIndex == 0 ? EQUIPMENT_SLOT_MAINHAND : EQUIPMENT_SLOT_OFFHAND;
        char const* hand = weaponIndex == 0 ? "main hand" : "off hand";

        // Same weapon still in that slot, and nothing applied to it since.
        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || weapon->GetGUID() != pending.item || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return;

        auto itr = poisonItemsByEnchant.find(pending.enchantId);
        if (itr == poisonItemsByEnchant.end())
            return;

        for (uint32 entry : itr->second)
        {
            if (!player->HasItemCount(entry, 1))
                continue;

            player->DestroyItemCount(entry, 1, true);
            SetPoison(player, weapon, pending.enchantId, PoisonDuration());

            if (config.poisonNotify)
                ChatHandler(player->GetSession()).PSendSysMessage("{} re-applied to your {} ({} left).",
                    ItemName(entry), hand, player->GetItemCount(entry));
            return;
        }

        if (config.poisonNotify)
            ChatHandler(player->GetSession()).PSendSysMessage("{} ran out on your {}; none left in your bags.",
                ItemName(itr->second.front()), hand);
    }

    void SendAddonMessage(Player* player, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, ADDON_PREFIX + text);
        player->SendDirectMessage(&data);
    }
}

namespace ClassTweaks
{
    void PoisonBuildTables()
    {
        poisonItemsByEnchant.clear();
        poisonEnchantBySpell.clear();
        poisonProcSpells.clear();

        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.Class != ITEM_CLASS_CONSUMABLE)
                continue;

            for (auto const& itemSpell : proto.Spells)
            {
                if (itemSpell.SpellId <= 0)
                    continue;

                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
                if (!spellInfo)
                    continue;

                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                {
                    if (spellInfo->Effects[i].Effect != SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                        continue;

                    uint32 enchantId = spellInfo->Effects[i].MiscValue;
                    uint32 procSpell = enchantId ? RoguePoisonProcSpell(enchantId) : 0;
                    if (!procSpell)
                        continue;

                    poisonItemsByEnchant[enchantId].push_back(entry);
                    poisonEnchantBySpell[spellInfo->Id] = enchantId;
                    poisonProcSpells.insert(procSpell);
                }
            }
        }

        size_t itemCount = 0;
        for (auto const& [enchantId, items] : poisonItemsByEnchant)
            itemCount += items.size();

        LOG_INFO("module", "mod-class-tweaks {}: {} rogue poison enchants found ({} poison items, {} proc spells).",
            CLASS_TWEAKS_VERSION, poisonItemsByEnchant.size(), itemCount, poisonProcSpells.size());
    }

    bool IsRoguePoisonProcSpell(uint32 spellId)
    {
        return poisonProcSpells.count(spellId) != 0;
    }

    std::unordered_set<uint32> const& RoguePoisonProcSpells()
    {
        return poisonProcSpells;
    }

    // After a poison was applied (runs after the spell's effects): give it the configured duration.
    void PoisonOnSpellCast(Player* player, Spell* spell, SpellInfo const* spellInfo)
    {
        if (!config.poisonDurationMs)
            return;

        auto itr = poisonEnchantBySpell.find(spellInfo->Id);
        if (itr == poisonEnchantBySpell.end())
            return;

        // Own items only: a weapon in the trade window isn't found here.
        Item* item = player->GetItemByGuid(spell->m_targets.GetItemTargetGUID());
        if (!item || item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != itr->second)
            return;

        SetPoison(player, item, itr->second, config.poisonDurationMs);
    }

    // Called by the core while an expiring temporary enchant is still on the item.
    void PoisonOnEnchantRemoved(Player* player, Item* item)
    {
        if (!item->IsEquipped() || item->GetBagSlot() != INVENTORY_SLOT_BAG_0)
            return;

        uint8 weaponIndex;
        switch (item->GetSlot())
        {
            case EQUIPMENT_SLOT_MAINHAND: weaponIndex = 0; break;
            case EQUIPMENT_SLOT_OFFHAND:  weaponIndex = 1; break;
            default: return;
        }

        uint32 enchantId = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (!poisonItemsByEnchant.count(enchantId) || !AutoApplyHere(player))
            return;

        PlayerState* state = GetState(player);
        state->pendingPoison[weaponIndex].item = item->GetGUID();
        state->pendingPoison[weaponIndex].enchantId = enchantId;
        state->hasPendingPoison = true;
    }

    void PoisonOnUpdate(Player* player, PlayerState* state)
    {
        // Dead: wait until alive again, unless auto-apply no longer applies here (left the instance).
        bool const allowed = AutoApplyHere(player);
        if (allowed && !player->IsAlive())
            return;

        state->hasPendingPoison = false;

        for (uint8 i = 0; i < 2; ++i)
        {
            PlayerState::PendingPoison pending = state->pendingPoison[i];
            state->pendingPoison[i] = PlayerState::PendingPoison();

            if (pending.enchantId && allowed)
                ReapplyPoison(player, i, pending);
        }
    }

    bool PoisonAutoApplyOn(Player* player)
    {
        if (!config.poisonAutoApply)
            return false;

        uint32 value = GetSetting(player, SETTING_AUTO_APPLY);
        return value != SETTING_OFF;  // not chosen = on
    }

    bool PoisonAutoApplyOutsideOn(Player* player)
    {
        if (!config.poisonAllowOutside || !PoisonAutoApplyOn(player))
            return false;

        uint32 value = GetSetting(player, SETTING_AUTO_APPLY_OUTSIDE);
        return value == SETTING_ON || (value != SETTING_OFF && config.poisonAutoApplyOutside);
    }

    void PoisonSetAutoApply(Player* player, bool on)
    {
        player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_AUTO_APPLY, on ? SETTING_ON : SETTING_OFF);
    }

    void PoisonSetAutoApplyOutside(Player* player, bool on)
    {
        player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_AUTO_APPLY_OUTSIDE, on ? SETTING_ON : SETTING_OFF);
    }

    // "STATE <protocol> <autoapply> <outside> <outsideAllowed>"
    void PoisonSendState(Player* player)
    {
        SendAddonMessage(player, Acore::StringFormat("STATE {} {} {} {}", ADDON_PROTOCOL,
            PoisonAutoApplyOn(player) ? 1 : 0, PoisonAutoApplyOutsideOn(player) ? 1 : 0, config.poisonAllowOutside ? 1 : 0));
    }

    // An addon message the player whispered to themselves. Returns false if it isn't ours.
    bool PoisonOnAddonMessage(Player* player, std::string const& msg)
    {
        if (msg.compare(0, ADDON_PREFIX.size(), ADDON_PREFIX) != 0)
            return false;

        // Only rogues with the feature on get an answer: no answer = the addon shows nothing.
        if (!config.poisonAutoApply || player->getClass() != CLASS_ROGUE)
            return true;

        std::string_view text = std::string_view(msg).substr(ADDON_PREFIX.size());

        if (text.rfind("HELLO", 0) == 0)
            PoisonSendState(player);
        else if (text == "SET AUTOAPPLY 0" || text == "SET AUTOAPPLY 1")
        {
            PoisonSetAutoApply(player, text.back() == '1');
            PoisonSendState(player);
        }
        else if (text == "SET OUTSIDE 0" || text == "SET OUTSIDE 1")
        {
            if (config.poisonAllowOutside)
                PoisonSetAutoApplyOutside(player, text.back() == '1');
            PoisonSendState(player);
        }

        return true;
    }
}
