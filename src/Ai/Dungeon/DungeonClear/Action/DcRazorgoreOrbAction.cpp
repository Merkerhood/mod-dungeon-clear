/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include <cmath>
#include <unordered_map>

#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"

// The orb runner's rung — see DungeonClearRazorgoreOrbAction in
// DungeonClearActions.h for what it is and why it is the runner's own tick
// rather than something the leader does to it.

namespace
{
    using namespace DcBlackwingLair;

    // Beyond this a bare MovePoint is not trusted to deliver: the engine
    // PathGenerator caps a generated path at 74 polys / 74 points and truncates
    // silently past that. The walk to the orb is ~78yd from the boss's spawn, so
    // it is a long haul every time. (Same number and same reason as the Violet
    // Hold driver's VH_LONG_HAUL.)
    constexpr float ORB_LONG_HAUL = 30.0f;

    // Same-destination re-issue floor. The orb never moves, so the only reason to
    // re-issue is that something layered a mover over our glide — in combat the
    // stock engine does exactly that whenever it wins a tick.
    constexpr uint32 ORB_REISSUE_MS = 1500;

    // "Is the glide in flight still aimed at the orb?" The destination is a fixed
    // point, so this only has to absorb the couple of yards of slop between the
    // requested point and where the route actually ends.
    constexpr float ORB_REPATH_EPSILON = 3.0f;

    // Walk the runner to the orb. Returns true when it issued movement (own the
    // tick), false when it is already there or a glide is riding correctly.
    bool OrbTravel(Player* bot, PlayerbotAI* botAI)
    {
        float const dist = bot->GetExactDist(ORB_X, ORB_Y, ORB_Z);

        MotionMaster* mm = bot->GetMotionMaster();
        float dx, dy, dz;
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
            mm->GetDestination(dx, dy, dz))
        {
            float const ex = dx - ORB_X, ey = dy - ORB_Y, ez = dz - ORB_Z;
            if (std::sqrt(ex * ex + ey * ey + ez * ez) <= ORB_REPATH_EPSILON)
                return true;  // already gliding here — let it ride, keep the tick
            DcMovement::ResolveEscortConflict(bot);
        }

        {
            thread_local std::unordered_map<uint32, uint32> lastIssueMs;
            uint32& prev = lastIssueMs[bot->GetGUID().GetCounter()];
            if (prev && GetMSTimeDiffToNow(prev) < ORB_REISSUE_MS)
                return true;
            prev = getMSTime();
        }

        if (dist > ORB_LONG_HAUL)
        {
            ChunkedPathfinder::Result const path =
                LongRangePathfinder::Build(bot, ORB_X, ORB_Y, ORB_Z);
            if (path.reachable && !path.segments.empty())
            {
                // Element 0 is the live position — the escort path[0]=start
                // convention SplinePath expects.
                Movement::PointsArray points;
                points.push_back(
                    G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
                for (PathSegment const& seg : path.segments)
                {
                    // The chamber is one bowl with a ramp up to the ledge, so a
                    // jump leg should never appear; deliver what we have if it does.
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
                {
                    LOG_DEBUG("playerbots.dungeonclear",
                              "DungeonClear: Razorgore — {} gliding to the orb ({:.1f}yd, {} pts)",
                              bot->GetName(), dist, uint32(points.size()));
                    return true;
                }
            }
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — {} has no long route to the orb ({:.1f}yd, {}) "
                      "-> falling back to a point move",
                      bot->GetName(), dist, path.failureReason);
        }

        bot->GetMotionMaster()->MovePoint(0, ORB_X, ORB_Y, ORB_Z, FORCED_MOVEMENT_NONE,
                                          /*speed*/ 0.0f, /*orientation*/ 0.0f,
                                          /*generatePath*/ true, /*forceDestination*/ false);
        return true;
    }
}

bool DungeonClearRazorgoreOrbTrigger::IsActive()
{
    // Map first: this is registered in both engines on every bot, and everywhere
    // outside Blackwing Lair it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcBlackwingLair::MAP_ID)
        return false;

    // The leader's election is the whole gate — it already checks that the run is
    // live and unpaused, and that this bot is the one member that may take the orb.
    return DcLeaderSignal::IsLeaderRazorgoreRunner(bot);
}

bool DungeonClearRazorgoreOrbAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    Creature* razor = bot->FindNearestCreature(NPC_RAZORGORE, ROOM_SCAN, /*alive*/ true);

    // Already driving him: the possession has us. Hold the ledge and hand the tick
    // back so the rotation runs — the bot is rooted by the charm anyway, and 90
    // seconds of a DPS standing mute is a real cost with an add landing every four
    // seconds. Only drift reclaims the tick.
    if (razor && razor->IsCharmed() && razor->GetCharmerGUID() == bot->GetGUID())
    {
        if (bot->GetExactDist2d(ORB_X, ORB_Y) > ORB_STATION_RADIUS * 2.0f)
            return OrbTravel(bot, botAI);
        return false;
    }

    if (bot->GetExactDist2d(ORB_X, ORB_Y) > ORB_STATION_RADIUS)
        return OrbTravel(bot, botAI);

    // Standing at the orb. Settle before clicking — a bot still coasting out of a
    // spline is moving, and the script's mind-control cast is an ordinary cast
    // that a moving, casting bot can lose.
    DcMovement::StopBot(bot, DcMovement::Stop::Hold);

    // The orb script's own refusals, re-read live rather than remembered: a pet
    // resummoned on the walk over and a lockout taken thirty seconds ago look
    // identical to a cached flag.
    if (bot->GetPet() || bot->HasAura(SPELL_MIND_EXHAUSTION) ||
        bot->IsNonMeleeSpellCast(false))
        return false;  // the leader's next election will move on to someone else

    if (!razor || razor->IsCharmed())
        return false;  // nothing to take, or somebody already has him

    GameObject* orb = bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, ROOM_SCAN);
    if (!orb)
        return false;

    LOG_INFO("playerbots.dungeonclear",
             "DungeonClear: Razorgore — {} clicks the Orb of Domination", bot->GetName());
    orb->Use(bot);
    return true;
}
