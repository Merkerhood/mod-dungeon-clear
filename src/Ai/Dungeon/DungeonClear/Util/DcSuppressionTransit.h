/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSUPPRESSIONTRANSIT_H
#define _PLAYERBOT_DCSUPPRESSIONTRANSIT_H

#include "Position.h"

class Player;
class PlayerbotAI;

// The movement half of Blackwing Lair's Suppression Rooms transit — the two
// things the leader's driver and every follower's pack rung both need, shared so
// the two halves can never disagree about them.
//
// It is a namespace of free functions rather than a class because there is no
// state here: both are pure geometry plus a spline issue, and the decision that
// USES them is the pure kernel in DcSuppressionTransitDecision.h.
namespace DcTransit
{
    // Walk `bot` to (x, y, z) unless it is already within `leash` of it.
    //
    // Long-haul aware and SAFE TO CALL EVERY TICK IN COMBAT, which is the whole
    // reason it exists as its own function. Three things have to be true at once
    // on this leg and none of them is free:
    //
    //   * A bare MovePoint past ~30yd fails SILENTLY — the engine PathGenerator
    //     caps a generated path at 74 polys / 74 points and truncates, leaving the
    //     bot standing still with no failure to observe at the call site. The
    //     authored legs run to 24yd and a straggler's catch-up is longer, so the
    //     long haul goes through LongRangePathfinder and a real spline.
    //   * There is NO `if (bot->isMoving()) return;` guard. In combat a bot is
    //     essentially always moving under MoveChase, so such a guard makes the
    //     whole transit a no-op for the entire crossing.
    //   * ...which means the re-issue floor is what stops it becoming spline spam,
    //     because the stock combat engine layers MoveChase back over the escort
    //     slot every tick it wins.
    //
    // Returns true when it issued (or is riding) movement, so the caller can own
    // the tick; false when the bot is already inside the leash.
    bool TravelTo(Player* bot, PlayerbotAI* botAI, float x, float y, float z, float leash);

    // Where to walk a bot the pack wants back: the point on the leash boundary
    // around `anchor` nearest to the bot, pulled `margin` INSIDE it — the near
    // EDGE, never the centre.
    //
    // Aiming at the centre is what the Razorgore camp's first cut did and what
    // CAMP_HOLD_MARGIN exists to prevent: a bot crosses the whole camp inward, the
    // fight pushes it back out, and it crosses again, forever, every bot out of
    // phase with the others. It matters MORE here, because this anchor advances
    // toward the bot as well — aiming at the centre of a moving leash makes every
    // correction a sprint past the leader instead of a step back into formation.
    //
    // Bearing is taken in 2D; z is INTERPOLATED along the same fraction. This used
    // to take the anchor's height outright, on the reasoning that the route is
    // authored on the floor the party walks — but the hold point is NOT the
    // anchor. It sits a leash back along the bearing, typically within a few yards
    // of the bot, so the anchor's floor height there describes a point that exists
    // nowhere the moment the leg is not flat; the crossing's ramps climb up to 5yd
    // over a single 20yd leg. Same defect the heal-reposition fallback carried, and
    // the same fix — see DungeonClearMath::PointTowardFrom.
    //
    // A follower genuinely off the route's floor (mid-fall, a tier up) is not
    // rescued by either choice: that is stranded recovery's rung, not this one.
    void HoldPoint(Player* bot, Position const& anchor, float leash, float margin,
                   float& hx, float& hy, float& hz);
}

#endif  // _PLAYERBOT_DCSUPPRESSIONTRANSIT_H
