/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcRouteFilter.h"

#include "Ai/Dungeon/DungeonClear/Data/DcNavPenaltyRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"   // dtPoly::getArea
#include "ObjectGuid.h"

#include <cmath>

DcRouteFilter::DcRouteFilter(uint32 mapId)
    : _mapId(mapId)
{
    // Server-only reads (Empty run-owner = plain conf/default, never a per-run
    // override). getCost runs on the path worker thread too, and these settings
    // are flagged non-overridable, so reading them straight from conf is safe off
    // the map thread — same contract as ApplyLiquidAreaCosts.
    _slopePenalty      = DcSettings::GetFloat(ObjectGuid::Empty, "SlopePathPenalty");
    _slopeThresholdDeg = DcSettings::GetFloat(ObjectGuid::Empty, "SlopePathThreshold");
    _hasVolumes        = DcNavPenaltyRegistry::HasVolumes(mapId);
}

float DcRouteFilter::getCost(float const* pa, float const* pb,
    dtPolyRef /*prevRef*/, dtMeshTile const* /*prevTile*/, dtPoly const* /*prevPoly*/,
    dtPolyRef /*curRef*/, dtMeshTile const* /*curTile*/, dtPoly const* curPoly,
    dtPolyRef /*nextRef*/, dtMeshTile const* /*nextTile*/, dtPoly const* /*nextPoly*/) const
{
    // Base cost mirrors dtQueryFilterExt: edge length × the poly's area cost (so
    // the liquid-avoidance setAreaCost multipliers still apply). The base class's
    // weak built-in slope term is replaced by the configurable ramp below.
    float const dist     = dtVdist(pa, pb);
    float const areaCost = getAreaCost(curPoly->getArea());

    // Detour vertex order is {y, z, x}: index 1 is elevation, 0 and 2 horizontal.
    float const horizX = pb[2] - pa[2];
    float const horizY = pb[0] - pa[0];
    float const horiz  = std::sqrt(horizX * horizX + horizY * horizY);
    float const rise   = std::fabs(pb[1] - pa[1]);

    float slopeMult = 1.0f;
    if (_slopePenalty > 0.0f && rise > 0.0f)
    {
        // Vertical-or-steeper edge with no run reads as a 90° pop.
        float const slopeDeg = (horiz > 0.01f)
            ? std::atan(rise / horiz) * 180.0f / float(M_PI)
            : 90.0f;
        if (slopeDeg > _slopeThresholdDeg)
            slopeMult = 1.0f + _slopePenalty * (slopeDeg - _slopeThresholdDeg) / 10.0f;
    }

    float volMult = 1.0f;
    if (_hasVolumes)
    {
        // Edge midpoint back in world space (undo Detour's {y, z, x}).
        float const wx = (pa[2] + pb[2]) * 0.5f;
        float const wy = (pa[0] + pb[0]) * 0.5f;
        float const wz = (pa[1] + pb[1]) * 0.5f;
        volMult = DcNavPenaltyRegistry::PenaltyAt(_mapId, wx, wy, wz);
    }

    return dist * areaCost * slopeMult * volMult;
}
