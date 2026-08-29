/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"

#include <algorithm>
#include <cmath>

#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"

#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"

namespace
{
    // Beyond this a bare MovePoint is not trusted to deliver — the silent
    // 74-poly truncation. Same number and same reason as the Violet Hold's
    // VH_LONG_HAUL and the orb runner's ORB_LONG_HAUL.
    constexpr float TRANSIT_LONG_HAUL = 30.0f;

    // Floor between two spline issues for the SAME destination. Out of combat the
    // escort-generator test below carries it alone; in combat it does not, because
    // MoveChase is layered back over the escort slot whenever the stock engine
    // wins a tick, and without a time floor this would rebuild a route every tick
    // for the whole crossing.
    constexpr uint32 TRANSIT_REISSUE_MS = 1500;

    // "Is the glide in flight still aimed at the right place?" — a CEILING, not a
    // constant. The cursor moves by one authored leg at a time (4-24yd), so a flat
    // epsilon wide enough to absorb route slop on a long leg would swallow a short
    // one entirely and the driver would issue nothing, for ever. Scaling it to
    // half the remaining trip (floor 2yd) makes the test narrower than the
    // movement it gates, always.
    constexpr float TRANSIT_REPATH_EPSILON = 10.0f;

    float RepathEpsilon(float dist)
    {
        return std::min(TRANSIT_REPATH_EPSILON, std::max(2.0f, dist * 0.5f));
    }

    // Below this the bot has no usable bearing from the anchor (it is effectively
    // standing on it), so there is no "near edge" to aim at. The caller gets the
    // anchor itself and the next tick, from a real bearing, does the holding.
    constexpr float BEARING_FLOOR = 1.0f;
}

bool DcTransit::TravelTo(Player* bot, PlayerbotAI* botAI, float x, float y, float z, float leash)
{
    if (!bot || !botAI)
        return false;

    float const dist = bot->GetExactDist(x, y, z);
    if (dist <= leash)
        return false;  // arrived — the caller must NOT own the tick

    float const epsilon = RepathEpsilon(dist);

    // An escort glide already in flight owns the bot. Let it finish if it is
    // headed here; drop it if it is stale — a route to the PREVIOUS cursor would
    // otherwise ride all the way out before the bot could react to the leg the
    // leader has actually moved on to.
    MotionMaster* mm = bot->GetMotionMaster();
    float dx, dy, dz;
    if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
        mm->GetDestination(dx, dy, dz))
    {
        float const ex = dx - x, ey = dy - y, ez = dz - z;
        if (std::sqrt(ex * ex + ey * ey + ez * ez) <= epsilon)
            return true;  // gliding here already — let it ride, keep the tick
        DcMovement::ResolveEscortConflict(bot);
    }

    // Same-destination re-issue floor: the test above cannot see a glide the
    // combat engine has since layered MoveChase over.
    //
    // Kept on the BOT'S OWN run state — see Util/DcThrottle.h for why not in a
    // file-scope thread_local map keyed by GUID.
    if (DcRun::Of(botAI).ThrottledIssue(DcThrottle::TransitIssue, x, y, z,
                                       epsilon, TRANSIT_REISSUE_MS))
        return true;

    if (dist > TRANSIT_LONG_HAUL)
    {
        ChunkedPathfinder::Result const path = LongRangePathfinder::Build(bot, x, y, z);
        if (path.reachable && !path.segments.empty())
        {
            // Element 0 is the live position — the escort path[0]=start convention
            // SplinePath expects.
            Movement::PointsArray points;
            points.push_back(
                G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
            for (PathSegment const& seg : path.segments)
            {
                // A jump leg cannot be expressed as a ground spline. The certified
                // corridor has a maximum vertical step of 2.6yd, so this should
                // never fire; deliver what we have if it does.
                if (seg.jumpDown || seg.jumpGap)
                    break;
                for (G3D::Vector3 const& p : seg.polyline)
                {
                    if (points.size() >= DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS)
                        break;
                    points.push_back(p);
                }
            }
            if (DcMovement::SplinePath(botAI, points))
                return true;
        }
        // No navmesh route: fall through. MovePoint will not deliver at this range
        // either, but it is a better last word than standing still.
    }

    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE,
                                      /*speed*/ 0.0f, /*orientation*/ 0.0f,
                                      /*generatePath*/ true, /*forceDestination*/ false);
    return true;
}

void DcTransit::HoldPoint(Player* bot, Position const& anchor, float leash, float margin,
                          float& hx, float& hy, float& hz)
{
    hx = anchor.GetPositionX();
    hy = anchor.GetPositionY();
    hz = anchor.GetPositionZ();
    if (!bot)
        return;

    // Never further out than the bot already is: the hold point pulls a bot IN,
    // and a bot inside the ring must not be pushed out to sit on it. z rides the
    // same fraction — see the header, and DungeonClearMath::PointTowardFrom.
    Position const hold = DungeonClearMath::PointTowardFrom(
        anchor, bot->GetPosition(), leash - margin, BEARING_FLOOR);
    hx = hold.GetPositionX();
    hy = hold.GetPositionY();
    hz = hold.GetPositionZ();
}
