/*
 * mod-class-tweaks (Rogue): combo points follow the rogue from target to target.
 *
 * Based on buildthehomelab/wow-mod-forever-rogue (MIT), rewritten to be event driven:
 *
 *   - A target change is handled when the client's CMSG_SET_SELECTION arrives (map thread),
 *     so the points are on the new target before the client can press a finisher. The original
 *     polled the selection every update, which left a one-update window where finishers failed.
 *   - A builder used on another unit (mouseover/focus) moves the points first (OnPlayerSpellCast,
 *     before the spell's effects).
 *   - The per-update check only reads two members unless the points just vanished, and runs for
 *     real rogues only. The original did an ObjectAccessor lookup and IsValidAttackTarget every
 *     update for every rogue, playerbots included, plus a heap-allocated DataMap key each time.
 *
 * Points that vanish because their target died (or despawned) wait ComboPoints.KeepAfterKill
 * milliseconds for the next hostile target. Finishers, duels ending, evade and Premeditation
 * running out leave the target alive, so those points are lost as in stock.
 */

#include "ClassTweaks.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <algorithm>

namespace
{
    using namespace ClassTweaks;

    // Points that vanish this soon after a finisher went to the finisher (Deadly Throw travels).
    constexpr std::chrono::milliseconds FINISHER_WINDOW{3000};

    std::chrono::milliseconds Now()
    {
        return GameTime::GetGameTimeMS();
    }

    void SavePoints(PlayerState* state, uint8 points)
    {
        if (!config.comboKeepAfterKill || !points)
            return;

        state->savedPoints = std::min<uint8>(points, 5);
        state->savedUntil = Now() + std::chrono::milliseconds(config.comboKeepAfterKill);
    }

    // Moves the rogue's live points, or points saved from a dead target, onto the new target.
    // Cheap checks first: most calls end before any lookup.
    void MovePointsTo(Player* player, PlayerState* state, Unit* target)
    {
        if (!target || target == player || player->GetComboTarget() == target)
            return;

        uint8 points = player->GetComboPoints();
        if (!points && !state->savedPoints)
            return;

        if (!target->IsAlive() || !player->IsValidAttackTarget(target))
            return;

        if (!points)
        {
            if (Now() > state->savedUntil)
            {
                state->savedPoints = 0;
                return;
            }
            points = state->savedPoints;
        }

        state->savedPoints = 0;

        // For a new target AddComboPoints sets the count rather than adding to it, moves the
        // holder and tells the client, which shows the points on the new target.
        player->AddComboPoints(target, points);

        state->lastPoints = points;
        state->lastTarget = target->GetGUID();
    }
}

namespace ClassTweaks
{
    void ComboOnSelect(Player* player, ObjectGuid guid)
    {
        if (!guid || !player->IsAlive())
            return;

        PlayerState* state = GetState(player);
        if (!player->GetComboPoints() && !state->savedPoints)
            return;

        MovePointsTo(player, state, ObjectAccessor::GetUnit(*player, guid));
    }

    // Runs before the spell's effects and before its last cast check.
    void ComboOnSpellCast(Player* player, Spell* spell)
    {
        SpellInfo const* spellInfo = spell->GetSpellInfo();

        if (spellInfo->NeedsComboPoints())
        {
            PlayerState* state = GetState(player);
            state->lastPoints = 0;
            state->savedPoints = 0;
            state->finisherUntil = Now() + FINISHER_WINDOW;
            return;
        }

        if (!spellInfo->HasEffect(SPELL_EFFECT_ADD_COMBO_POINTS))
            return;

        if (Unit* target = spell->m_targets.GetUnitTarget())
            MovePointsTo(player, GetState(player), target);
    }

    void ComboOnUpdate(Player* player, PlayerState* state)
    {
        uint8 const points = player->GetComboPoints();
        Unit* comboTarget = player->GetComboTarget();

        if (points && comboTarget)
        {
            if (comboTarget->IsAlive())
            {
                state->lastPoints = points;
                state->lastTarget = comboTarget->GetGUID();
                return;
            }

            // Points on a corpse: a builder killed its target. The death cleared the points the
            // rogue had, then the builder's points were added to the corpse from zero. Put both
            // back together and let them wait for the next target.
            uint8 total = points;
            if (state->lastTarget == comboTarget->GetGUID())
                total += state->lastPoints;

            state->lastPoints = 0;
            state->lastTarget.Clear();
            player->ClearComboPoints();
            SavePoints(state, total);
            return;
        }

        if (state->lastPoints)
        {
            // The points are gone. Keep them only if their target died or despawned.
            if (Now() >= state->finisherUntil)
            {
                Unit* target = ObjectAccessor::GetUnit(*player, state->lastTarget);
                if (!target || !target->IsAlive())
                    SavePoints(state, state->lastPoints);
            }

            state->lastPoints = 0;
            state->lastTarget.Clear();

            // The rogue may already have a living target selected (points were on a
            // mouseover/focus target that died).
            if (state->savedPoints)
                MovePointsTo(player, state, player->GetSelectedUnit());
            return;
        }

        if (state->savedPoints && Now() > state->savedUntil)
            state->savedPoints = 0;
    }
}
