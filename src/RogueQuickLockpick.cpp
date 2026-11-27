/*
 * mod-class-tweaks (Rogue): open a lockbox in one step.
 *
 * Based on buildthehomelab/wow-mod-quick-lockpick (MIT). Behaviour is unchanged:
 *
 *   right-click a locked box  Pick Lock is cast on it, with every normal check (cast time,
 *                             moving interrupts it, skill too low fails with the usual message)
 *   the box comes unlocked    it opens, as a second right-click would
 *
 * The client grays an item out when it sends CMSG_OPEN_ITEM and only gives it back on a loot
 * response or a loot release for it, so a right-click Pick Lock that doesn't end in the loot
 * window sends the release.
 *
 * Changes from the original: playerbots are excluded through the shared check (which fails the
 * build instead of silently including bots if the core renames it), the all-spells hook rejects
 * non-players before looking at spell effects, and the state lives in the module's single
 * per-player DataMap entry.
 */

#include "ClassTweaks.h"
#include "DBCStores.h"
#include "Item.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"

namespace
{
    using namespace ClassTweaks;

    constexpr uint32 SPELL_PICK_LOCK = 1804;

    // Backpack and bags, not the bank.
    bool IsInBags(Item const* item)
    {
        return Player::IsInventoryPos(item->GetBagSlot(), item->GetSlot());
    }

    // A box with loot that's still locked. Wrapped gifts and other openable items don't count.
    bool IsLockedBox(Item const* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        return proto && proto->LockID && proto->HasFlag(ITEM_FLAG_HAS_LOOT) && !item->IsWrapped() && item->IsLocked();
    }

    // Whether Pick Lock is one of the ways to open this lock. Boxes that need a key keep the
    // stock "Item is locked".
    bool IsPickable(uint32 lockId)
    {
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] == LOCKTYPE_PICKLOCK)
                return true;

        return false;
    }

    // What the core's CMSG_OPEN_ITEM handler does with an unlocked box. Other modules'
    // OnPlayerBeforeOpenItem hooks get their say.
    bool OpenBox(Player* player, Item* item)
    {
        if (!player->IsAlive() || !sScriptMgr->OnPlayerBeforeOpenItem(player, item))
            return false;

        player->SendLoot(item->GetGUID(), LOOT_CORPSE);
        return true;
    }

    // The box an open-lock cast is aimed at, if it's in the caster's own bags. Looked up again by
    // GUID: the spell's raw item pointer can outlive an item destroyed mid-cast. A box someone
    // else put in the trade window isn't found in the caster's inventory and is skipped.
    Item* OwnBoxTarget(Spell* spell, Player* player)
    {
        if (!(spell->m_targets.GetTargetMask() & TARGET_FLAG_ITEM))
            return nullptr;

        Item* item = player->GetItemByGuid(spell->m_targets.GetItemTargetGUID());
        if (!item || item->IsInTrade() || !IsInBags(item))
            return nullptr;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !proto->LockID || !proto->HasFlag(ITEM_FLAG_HAS_LOOT) || item->IsWrapped())
            return nullptr;

        return item;
    }

    // Ends a right-click Pick Lock on this box. Without the loot window the client needs a release.
    void FinishPendingPick(Player* player, ObjectGuid box, bool opened)
    {
        if (!box)
            return;

        PlayerState* state = GetState(player);
        if (state->pendingBox != box)
            return;

        state->pendingBox.Clear();
        if (!opened)
            player->SendLootRelease(box);
    }
}

namespace ClassTweaks
{
    // CMSG_OPEN_ITEM. Returns false when this module cast Pick Lock in place of the core handler.
    bool LockpickOnOpenItem(Player* player, WorldPacket const& packet)
    {
        if (packet.size() < 2 || !player->IsAlive() || player->m_mover != player)
            return true;

        // Something else is being cast (Hearthstone, a craft, Pick Lock itself): leave it be.
        if (!player->HasSpell(SPELL_PICK_LOCK) || player->IsNonMeleeSpellCast(false, false, true))
            return true;

        // CMSG_OPEN_ITEM is bag, slot. Read in place; the core's handler reads the packet itself.
        uint8 bag = packet.read<uint8>(0);
        uint8 slot = packet.read<uint8>(1);

        Item* item = player->GetItemByPos(bag, slot);
        if (!item || item->IsInTrade() || !IsInBags(item) || !IsLockedBox(item) || !IsPickable(item->GetTemplate()->LockID))
            return true;

        SpellInfo const* pickLock = sSpellMgr->GetSpellInfo(SPELL_PICK_LOCK);
        if (!pickLock)
            return true;

        SpellCastTargets targets;
        targets.SetItemTarget(item);

        // Marked before the cast: a cast with no cast time finishes inside CastSpell. A failed
        // cast already shows its reason; give the grayed-out box back.
        ObjectGuid box = item->GetGUID();
        PlayerState* state = GetState(player);
        state->pendingBox = box;
        if (player->CastSpell(targets, pickLock, nullptr, TRIGGERED_NONE) != SPELL_CAST_OK)
        {
            state->pendingBox.Clear();
            player->SendLootRelease(box);
        }

        return false;
    }

    // Runs after the cast's effects, so a box Pick Lock or a key just opened is unlocked by now.
    void LockpickOnSpellCast(Player* player, Spell* spell, SpellInfo const* spellInfo)
    {
        if (!spellInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK))
            return;

        bool opened = false;
        if (config.lockpickOpenAfterUnlock)
            if (Item* item = OwnBoxTarget(spell, player))
                if (!item->IsLocked())
                    opened = OpenBox(player, item);

        FinishPendingPick(player, spell->m_targets.GetItemTargetGUID(), opened);
    }

    // A right-click Pick Lock that was interrupted: give the grayed-out box back. Only the GUID
    // is used; the item may already be gone.
    void LockpickOnSpellCastCancel(Player* player, Spell* spell)
    {
        if (spell->GetSpellInfo()->Id == SPELL_PICK_LOCK)
            FinishPendingPick(player, spell->m_targets.GetItemTargetGUID(), false);
    }
}
