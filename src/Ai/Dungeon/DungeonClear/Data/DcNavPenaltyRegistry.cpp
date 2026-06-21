/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNavPenaltyRegistry.h"

#include <array>

namespace
{
    // ---- the table ------------------------------------------------------
    // One row per navmesh shortcut a real player can't follow.
    //
    // Lower Blackrock Spire (map 229). The navmesh stitches a walkable poly up
    // the chasm wall between the lower walkway (~z30) and an upper ledge (~z58),
    // so the tank climbs a near-vertical face the party can't follow — skipping
    // the intended route around. Observed bot traversal:
    //     [-127.33, -402.11, 30.32]  ->  [-124.88, -378.42, 58.40]
    // (≈28yd of rise over ≈24yd of ground = ~50°, right at the walkable limit).
    //
    // The box hugs the vertical shaft and deliberately spans only the MIDDLE Z
    // band (33..56): the legitimate floor at the bottom and the ledge platform at
    // the top sit outside it, so a route that genuinely belongs down there or up
    // there is not taxed — only an edge that is climbing the wall pays. A stiff
    // multiplier makes the A* corridor take the real way around whenever one
    // exists; it stays a cost, so the spot is never made unreachable.
    constexpr std::array<DcNavPenaltyVolume, 1> kVolumes = {{
        { 229, -134.0f, -406.0f, 33.0f, -118.0f, -374.0f, 56.0f, 40.0f },
    }};
}

bool DcNavPenaltyRegistry::HasVolumes(uint32 mapId)
{
    for (auto const& v : kVolumes)
        if (v.mapId == mapId)
            return true;
    return false;
}

float DcNavPenaltyRegistry::PenaltyAt(uint32 mapId, float x, float y, float z)
{
    float worst = 1.0f;
    for (auto const& v : kVolumes)
    {
        if (v.mapId != mapId)
            continue;
        if (x < v.minX || x > v.maxX)
            continue;
        if (y < v.minY || y > v.maxY)
            continue;
        if (z < v.minZ || z > v.maxZ)
            continue;
        if (v.costMult > worst)
            worst = v.costMult;
    }
    return worst;
}
