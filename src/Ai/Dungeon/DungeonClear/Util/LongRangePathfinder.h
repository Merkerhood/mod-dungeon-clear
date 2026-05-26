/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_LONGRANGEPATHFINDER_H
#define _PLAYERBOT_LONGRANGEPATHFINDER_H

#include "Common.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"  // PathSegment, ChunkedPathfinder::Result

class Player;

// LongRangePathfinder computes the WHOLE smoothed route to a target in one
// shot, bypassing the engine PathGenerator's 74-poly / 74-point caps that
// forced StridedPathfinder to chunk long routes into ~180yd strides.
//
// It runs Detour directly against the map's navmesh using its OWN
// dtNavMeshQuery initialised with a large search-node pool (the shared
// engine query is only 1024 nodes — far too small for a dungeon-length A*,
// and we must not perturb it since every creature's movement uses it). The
// poly corridor and smoothed point buffers are sized for an entire dungeon,
// so total distance no longer matters: the result is a single PathSegment
// whose polyline is the full smoothed corridor, which DungeonPathFollower
// already walks point-by-point.
//
// This is the PRIMARY producer behind StridedPathfinder::Build. When it can
// build a reachable route the caller returns it directly; when it can't
// (no navmesh, target off-mesh, immediate static obstruction) the caller
// falls through to the hardened bee-line / arc / spawn-graph stride tiers.
//
// Main-thread only. Detour's path/smooth queries read the static navmesh and
// touch no live game state, so this is trivially movable to a worker thread
// later if the server ever needs to route more than one bot at a time.
class LongRangePathfinder
{
public:
    // Same Result/PathSegment shape as the strided builder so the value
    // cache (DungeonClearLongPathValue) and follower stay unchanged.
    using Result = ChunkedPathfinder::Result;

    // Build the full smoothed route from the bot's current position to
    // (tx,ty,tz). The target is snapped to the navmesh internally. On
    // success the Result carries one PathSegment whose polyline is the
    // entire corridor (leading start point dropped, matching the
    // PathSegment convention). reachable/complete follow the same meaning
    // as StridedPathfinder. On any failure reachable is false and
    // failureReason explains why; startFarFromPoly is set when the bot
    // itself is off the navmesh so Advance can run a recovery hop.
    static Result Build(Player* bot, float tx, float ty, float tz);
};

#endif
