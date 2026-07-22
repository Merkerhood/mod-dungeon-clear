/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcNavPenaltyRegistry.h"

#include <array>

namespace
{
    // ---- the table ------------------------------------------------------
    // One row per navmesh shortcut a real player can't follow. Each box spans
    // only the MIDDLE Z band of its climb — the legitimate floor below and ledge
    // platform above sit just outside it, so a route that genuinely belongs down
    // there or up there is untaxed; only an edge climbing the face pays. A stiff
    // multiplier makes the A* corridor take the real way around whenever one
    // exists; it stays a cost, so the spot is never made unreachable.
    //
    // Lower Blackrock Spire (map 229), #1 — the big chasm climb. The navmesh
    // stitches a walkable poly up the wall between the lower walkway (~z30) and an
    // upper ledge (~z58), so the tank climbs a near-vertical face the party can't
    // follow. Observed bot traversal:
    //     [-127.33, -402.11, 30.32]  ->  [-124.88, -378.42, 58.40]
    // (≈28yd rise over ≈24yd ground = ~50°). Box mid-band z 33..56.
    //
    // Lower Blackrock Spire (map 229), #2 — a small ledge-hop further along the
    // (now-corrected) route, where some bots wedged on the step. Same class, much
    // smaller. Observed traversal:
    //     [-61.70, -382.77, 48.88]  <->  [-64.34, -378.49, 54.70]
    // (≈5.8yd rise over ≈5yd ground = ~49°). Tight box hugging the two endpoints,
    // mid-band z 49.4..54.2 so the lower walkway (≤~49) and the upper platform
    // (≥~54.7, which the proper route reaches from another direction) stay untaxed.
    //
    // Sethekk Halls (map 556) — the Talon King's back-door ramp. The instance is a
    // loop: Talon King Ikiss (44.7, 287.0, z25) sits on an upper ring you reach the
    // long way (west ramp near (-250, 210) up to z27, then east across the upper
    // room). But a narrow ramp climbs straight to his platform from a closed,
    // script-controlled door (GO 183398 at (44.8, 150.7, z0)) on the LOWER level
    // directly south of him. The mmap stitches that ramp into the navmesh ignoring
    // the shut door, so Detour's A* picks the short south climb, routes the party
    // back toward the entrance, and wedges at the door (it can't open it) — the
    // "can't reach Ikiss after Syth" symptom. The ramp is a single ~x45 column
    // (x≈40..50, void either side) rising y153->245, z0->27, opening onto the
    // shared platform only at y>=250. Box the whole climb up to (not into) the
    // platform — x 25..68, y 150..248, z -5..30 — so every edge on the shortcut is
    // taxed while the upper room / platform (y>=250) and the lower lobby (y<150)
    // stay untaxed and the legitimate west-ramp route is left at base cost.
    // The Arcatraz (map 552) — the five Arcatraz Sentinel (20869) spawns. NOT a
    // navmesh shortcut: this is the route half of DcHazardRegistry. Each rooted
    // Sentinel pulses 563-937 damage in 15yd every second, forever, so the
    // router should prefer a line that hugs the far wall. Boxes are the emitter
    // position +-22 in XY and +-12 in Z (the registry's radius / zBand), matching
    // DcHazardRegistry's rows so the route half and the live half agree.
    //
    // COST MULTIPLIER IS 8, NOT 40, AND HAZARDS ARE DELIBERATELY NOT WIRED INTO
    // THE StridedPathfinder HARD REJECT. Sentinels 138931 (255.5,158.9) and
    // 138932 (253.9,131.9) sit 27yd apart in what is the only corridor through
    // that stretch of the Containment Core: their boxes overlap and span it.
    // A cost is survivable there (the route still goes through, just last) —
    // a rejection would strand the party. 8 is enough to bend a route around an
    // emitter when floor space exists, without making an unavoidable corridor
    // rank worse than a genuinely broken navmesh shortcut at 40.
    //
    // If test runs show bots still walking the pulse, NARROW THE BOXES rather
    // than raising the multiplier — a wider tax on a mandatory corridor buys
    // nothing and starts competing with the shortcut rows above.
    constexpr std::array<DcNavPenaltyVolume, 8> kVolumes = {{
        { 229, -134.0f, -406.0f, 33.0f, -118.0f, -374.0f, 56.0f, 40.0f },
        { 229,  -65.5f, -384.0f, 49.4f,  -60.5f, -377.0f, 54.2f, 40.0f },
        { 556,   25.0f,  150.0f, -5.0f,   68.0f,  248.0f, 30.0f, 40.0f },
        { 552,  233.5f,  136.9f, 10.4f,  277.5f,  180.9f, 34.4f,  8.0f },
        { 552,  231.9f,  109.9f, 10.4f,  275.9f,  153.9f, 34.4f,  8.0f },
        { 552,  242.3f,  -83.3f, 10.5f,  286.3f,  -39.3f, 34.5f,  8.0f },
        { 552,  314.5f,    5.4f, 36.4f,  358.5f,   49.4f, 60.4f,  8.0f },
        { 552,  373.4f,   -3.8f, 36.3f,  417.4f,   40.2f, 60.3f,  8.0f },
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
