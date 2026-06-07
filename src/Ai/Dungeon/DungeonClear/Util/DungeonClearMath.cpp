/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearMath.h"

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
