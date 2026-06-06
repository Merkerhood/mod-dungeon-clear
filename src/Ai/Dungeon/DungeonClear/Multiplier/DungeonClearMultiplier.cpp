/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMultiplier.h"

#include "Action.h"
#include "FollowActions.h"
#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

float DungeonClearMultiplier::GetValue(Action* action)
{
    if (!action || !botAI || !bot)
        return 1.0f;

    std::string const& name = action->getName();

    // Rest-target cap. Applies to EVERY bot in an active DC run — the leader tank
    // AND its followers — so the whole group stops eating/drinking at the group's
    // chosen target (DungeonClear.RestHealthPct / RestManaPct). Followers never
    // set `enabled`, so we gate on the cross-bot "dungeon clear party tank" value
    // (non-null only while the leader's clear runs and is unpaused) rather than
    // the per-bot enabled flag used below. The matching
    // DungeonClearNeeds{Eat,Drink} triggers raise the floor (drink/eat up to the
    // target even above the stock playerbots stop); this caps the ceiling so a
    // target BELOW the stock stop is honoured too. 0 = inherit, no cap.
    if (name == "food" || name == "drink")
    {
        if (AI_VALUE(Player*, "dungeon clear party tank"))
        {
            bool const isDrink = (name == "drink");
            uint32 const target =
                DcSettings::GetUInt(bot, isDrink ? "RestManaPct" : "RestHealthPct");
            if (target > 0)
            {
                float const pct =
                    isDrink ? bot->GetPowerPct(POWER_MANA) : bot->GetHealthPct();
                if (pct >= static_cast<float>(target))
                    return 0.0f;
            }
        }
    }

    // While paused, behave as if DC were off: don't suppress wandering, so the
    // tank reverts to stock non-combat behavior just like under `dc off`.
    if (!AI_VALUE(bool, "dungeon clear enabled") || AI_VALUE(bool, "dungeon clear paused"))
        return 1.0f;

    // The tank leads the clear — it must never follow its master. When Advance
    // yields to wait for the party to catch up (party spread > DungeonClear.PartyMaxSpread)
    // it StopMoving()s and parks; without this, the stock FollowAction (relevance
    // 1.0) then wins the idle tick and walks the tank BACK toward the stationary
    // player, who is now in range again, so next tick Advance runs it forward to
    // the spread limit and it yields again — a rubberband between the spread limit
    // and the player. Suppressing follow for the tank lets it simply hold at the
    // spread limit until the player catches up. Followers are unaffected: their
    // redirect (DungeonClearFollowTankAction) is a separate MovementAction, and
    // only non-tanks run it.
    if (PlayerbotAI::IsTank(bot) && dynamic_cast<FollowAction*>(action))
        return 0.0f;

    // Suppress wander-style actions while DC is active. Anything else
    // (loot, food, drink, combat, our own dungeon-clear actions, and follow
    // for non-tanks) is untouched.
    if (name.find("grind") != std::string::npos)
        return 0.0f;
    if (name.find("rpg") != std::string::npos)
        return 0.0f;
    if (name == "travel" || name.find("travel ") != std::string::npos)
        return 0.0f;
    return 1.0f;
}
