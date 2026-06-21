/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCROUTEFILTER_H
#define _PLAYERBOT_DCROUTEFILTER_H

#include "Define.h"
#include "DetourExtended.h"   // dtQueryFilterExt

// Route-cost filter for the dungeon-clear long-range pathfinder. Extends the
// engine's dtQueryFilterExt with two human-authored "discouragements" that steer
// the A* corridor away from navmesh shortcuts a real player cannot follow:
//
//   1. A configurable STEEP-SLOPE penalty. The stock dtQueryFilterExt::getCost
//      barely taxes slope (a ~50° climb costs only ~1.5×), so Detour happily
//      routes the bot up near-vertical walls the mmap generator left walkable.
//      Above a threshold this ramps the per-edge cost so the search prefers a
//      flatter way around whenever one exists. GENERAL: discourages every
//      wall-climb shortcut, not just the spots we've found by hand.
//
//   2. A no-go VOLUME penalty from DcNavPenaltyRegistry. SURGICAL: a hand-
//      authored axis-aligned box over a known-bad spot (e.g. the LBRS chasm
//      climb) multiplies any edge through it so heavily that the detour always
//      wins — the reliable kill for a spot the slope rule alone can't catch
//      (gentle portal-to-portal slope) or that we want guaranteed.
//
// Both are cost multipliers layered on the base distance × area-cost; neither
// touches passFilter, so a discouraged edge stays traversable. If a steep climb
// or a boxed edge is genuinely the ONLY way through, the route still takes it —
// the bot is never stranded by these rules, only nudged off avoidable shortcuts.
//
// One instance per Build (cheap: a few config reads in the ctor). getAreaCost /
// include/exclude flags are inherited unchanged, so callers still apply the
// liquid-avoidance area costs (DungeonClearGeometry::ApplyLiquidAreaCosts) on top.
class DcRouteFilter : public dtQueryFilterExt
{
public:
    explicit DcRouteFilter(uint32 mapId);

    float getCost(float const* pa, float const* pb,
        dtPolyRef prevRef, dtMeshTile const* prevTile, dtPoly const* prevPoly,
        dtPolyRef curRef, dtMeshTile const* curTile, dtPoly const* curPoly,
        dtPolyRef nextRef, dtMeshTile const* nextTile, dtPoly const* nextPoly) const override;

private:
    uint32 _mapId;
    float  _slopePenalty;        // ramp strength above the threshold (0 = off)
    float  _slopeThresholdDeg;   // slope at/below this is untaxed
    bool   _hasVolumes;          // map has a no-go volume (per-edge box-test gate)
};

#endif
