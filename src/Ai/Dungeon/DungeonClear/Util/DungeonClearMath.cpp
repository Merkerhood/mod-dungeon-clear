/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMath.h"

#include <cmath>
#include <cstddef>

bool DungeonClearMath::ClassifyDynamicPull(std::vector<DynPullMob> const& mobs,
                                           std::size_t targetIdx, float packRadius,
                                           float chainRadius,
                                           std::uint32_t largePackThreshold,
                                           float zTolerance)
{
    std::size_t const n = mobs.size();
    if (n == 0 || targetIdx >= n)
        return false;

    auto dist2d = [&](std::size_t a, std::size_t b)
    {
        float const dx = mobs[a].x - mobs[b].x;
        float const dy = mobs[a].y - mobs[b].y;
        return std::sqrt(dx * dx + dy * dy);
    };
    // Same floor: the inter-mob height gap is within tolerance. WotLK floor-to-
    // floor gaps comfortably exceed it, so this separates a ledge/ramp pack
    // overhead from one the tank can actually walk into.
    auto sameLevel = [&](std::size_t a, std::size_t b)
    {
        return std::fabs(mobs[a].z - mobs[b].z) <= zTolerance;
    };

    // Union-Find connected components. Two mobs join the same pack when either:
    //   - they are within packRadius (2D) AND on the same level (geometry), or
    //   - they share a non-zero packId (an engine formation/link unites them as
    //     one unit regardless of spacing or height — a strung-out formation reads
    //     as one pack, not several lone mobs).
    // n is tiny, so O(n^2) is fine.
    std::vector<std::size_t> parent(n);
    for (std::size_t i = 0; i < n; ++i)
        parent[i] = i;
    auto find = [&parent](std::size_t a)
    {
        while (parent[a] != a)
        {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    auto samePack = [&](std::size_t a, std::size_t b)
    {
        return mobs[a].packId != 0 && mobs[a].packId == mobs[b].packId;
    };
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
            if ((dist2d(i, j) <= packRadius && sameLevel(i, j)) || samePack(i, j))
                parent[find(i)] = find(j);

    std::size_t const targetRoot = find(targetIdx);

    // Size override: a big lone pack still earns the careful Advanced pull.
    std::uint32_t packSize = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (find(i) == targetRoot)
            ++packSize;
    if (packSize > largePackThreshold)
        return true;

    // Threatening neighbours: an OTHER pack with a chainEligible mob within
    // chainRadius (2D) and on the same level as any target-pack mob. A neighbour
    // directly above/below is a different floor's problem, not this pull's.
    for (std::size_t i = 0; i < n; ++i)
    {
        if (find(i) == targetRoot || !mobs[i].chainEligible)
            continue;
        for (std::size_t j = 0; j < n; ++j)
        {
            if (find(j) != targetRoot)
                continue;
            if (dist2d(i, j) <= chainRadius && sameLevel(i, j))
                return true;
        }
    }
    return false;
}

float DungeonClearMath::DistSqToSegment2D(float px, float py,
                                         float ax, float ay,
                                         float bx, float by)
{
    float const ex = bx - ax;
    float const ey = by - ay;
    float const len2 = ex * ex + ey * ey;
    if (len2 <= 1e-6f)
    {
        float const dx = px - ax;
        float const dy = py - ay;
        return dx * dx + dy * dy;
    }
    float t = ((px - ax) * ex + (py - ay) * ey) / len2;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    float const cx = ax + t * ex;
    float const cy = ay + t * ey;
    float const dx = px - cx;
    float const dy = py - cy;
    return dx * dx + dy * dy;
}

bool DungeonClearMath::SegmentIntersectsAABB2D(float ax, float ay,
                                               float bx, float by,
                                               float minX, float minY,
                                               float maxX, float maxY)
{
    // Parametric segment P(t) = A + t*(B-A), t in [0,1]. Clip the interval
    // against each box slab; if anything survives, the segment touches the box.
    float const dx = bx - ax;
    float const dy = by - ay;
    float t0 = 0.0f;
    float t1 = 1.0f;

    // p = -direction component, q = distance to the slab edge. The pair
    // (-dx, ax-minX), (dx, maxX-ax), (-dy, ay-minY), (dy, maxY-ay) covers the
    // four edges (left, right, bottom, top).
    float const p[4] = {-dx, dx, -dy, dy};
    float const q[4] = {ax - minX, maxX - ax, ay - minY, maxY - ay};

    for (int i = 0; i < 4; ++i)
    {
        if (p[i] == 0.0f)
        {
            // Segment parallel to this slab: outside it -> no intersection.
            if (q[i] < 0.0f)
                return false;
            continue;
        }
        float const r = q[i] / p[i];
        if (p[i] < 0.0f)
        {
            if (r > t1)
                return false;
            if (r > t0)
                t0 = r;
        }
        else
        {
            if (r < t0)
                return false;
            if (r < t1)
                t1 = r;
        }
    }
    return t0 <= t1;
}
