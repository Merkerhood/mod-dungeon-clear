/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonPathFollower.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "Player.h"

namespace
{
    bool IsJumpSegment(PathSegment const& seg)
    {
        return seg.jumpDown || seg.jumpGap;
    }

    bool IsLastPointOfSegment(PathSegment const& seg, uint32 pointIdx)
    {
        return !seg.polyline.empty() && pointIdx + 1 == seg.polyline.size();
    }

    // Returns the polyline point at (segIdx, pointIdx) or nullopt if out
    // of range. Skips empty segments by walking forward.
    std::optional<G3D::Vector3> PointAt(ChunkedPathfinder::Result const& path, uint32 segIdx, uint32 pointIdx)
    {
        if (segIdx >= path.segments.size())
            return std::nullopt;
        std::vector<G3D::Vector3> const& poly = path.segments[segIdx].polyline;
        if (pointIdx >= poly.size())
            return std::nullopt;
        return poly[pointIdx];
    }

    // Returns the polyline point that comes BEFORE (segIdx, pointIdx),
    // crossing segment boundaries as needed. Used by IsOffPath to anchor
    // the previous end of the current line segment.
    std::optional<G3D::Vector3> PrevPoint(ChunkedPathfinder::Result const& path, uint32 segIdx, uint32 pointIdx)
    {
        if (pointIdx > 0)
            return PointAt(path, segIdx, pointIdx - 1);
        // pointIdx == 0; walk back to the previous non-empty segment's last point.
        for (uint32 i = segIdx; i > 0; --i)
        {
            std::vector<G3D::Vector3> const& prevPoly = path.segments[i - 1].polyline;
            if (!prevPoly.empty())
                return prevPoly.back();
        }
        return std::nullopt;
    }

    // Returns the polyline point that comes AFTER (segIdx, pointIdx),
    // crossing segment boundaries as needed.
    std::optional<G3D::Vector3> NextPoint(ChunkedPathfinder::Result const& path, uint32 segIdx, uint32 pointIdx)
    {
        std::optional<G3D::Vector3> p = PointAt(path, segIdx, pointIdx + 1);
        if (p.has_value())
            return p;
        // No further point in this segment — walk forward to the next non-empty segment's first point.
        for (uint32 i = segIdx + 1; i < path.segments.size(); ++i)
        {
            std::vector<G3D::Vector3> const& poly = path.segments[i].polyline;
            if (!poly.empty())
                return poly.front();
        }
        return std::nullopt;
    }

    // Squared 2D distance from (px, py) to the line segment a→b. Used so
    // we can compare without taking square roots in the hot path.
    float Dist2ToSegment2DSq(float px, float py, G3D::Vector3 const& a, G3D::Vector3 const& b)
    {
        float const ex = b.x - a.x;
        float const ey = b.y - a.y;
        float const len2 = ex * ex + ey * ey;
        if (len2 <= 1e-6f)
        {
            float const dx = px - a.x;
            float const dy = py - a.y;
            return dx * dx + dy * dy;
        }
        float t = ((px - a.x) * ex + (py - a.y) * ey) / len2;
        t = std::max(0.0f, std::min(1.0f, t));
        float const cx = a.x + t * ex;
        float const cy = a.y + t * ey;
        float const dx = px - cx;
        float const dy = py - cy;
        return dx * dx + dy * dy;
    }

    // Advances (segIdx, pointIdx) by one polyline point, skipping over
    // any empty segments. Returns false when there is nothing left.
    bool AdvanceCursor(ChunkedPathfinder::Result const& path, uint32& segIdx, uint32& pointIdx)
    {
        if (segIdx >= path.segments.size())
            return false;
        ++pointIdx;
        while (segIdx < path.segments.size() && pointIdx >= path.segments[segIdx].polyline.size())
        {
            ++segIdx;
            pointIdx = 0;
        }
        return segIdx < path.segments.size();
    }
}

DungeonPathFollower::Hop DungeonPathFollower::NextHop(Player* bot, ChunkedPathfinder::Result const& path,
                                                     DungeonFollowerState& state)
{
    Hop hop;
    if (!bot || path.segments.empty())
    {
        hop.isDone = true;
        return hop;
    }

    // Skip over empty segments at the start.
    while (state.segmentIdx < path.segments.size() &&
           state.pointIdx >= path.segments[state.segmentIdx].polyline.size())
    {
        ++state.segmentIdx;
        state.pointIdx = 0;
    }

    // Advance past any points the bot has already reached. Cheap loop —
    // typically zero iterations per tick, but catches the "engine spline
    // sped through several polyline points between Advance ticks" case.
    while (state.segmentIdx < path.segments.size())
    {
        std::optional<G3D::Vector3> cur = PointAt(path, state.segmentIdx, state.pointIdx);
        if (!cur.has_value())
        {
            hop.isDone = true;
            return hop;
        }
        float const d = bot->GetDistance(cur->x, cur->y, cur->z);
        if (d > POINT_REACHED)
        {
            hop.point = *cur;
            PathSegment const& seg = path.segments[state.segmentIdx];
            hop.isJump = IsJumpSegment(seg) && IsLastPointOfSegment(seg, state.pointIdx);
            return hop;
        }
        // Reached this point — advance cursor.
        if (!AdvanceCursor(path, state.segmentIdx, state.pointIdx))
        {
            hop.isDone = true;
            return hop;
        }
    }

    hop.isDone = true;
    return hop;
}

bool DungeonPathFollower::IsOffPath(Player* bot, ChunkedPathfinder::Result const& path, DungeonFollowerState& state)
{
    if (!bot || path.segments.empty() || state.segmentIdx >= path.segments.size())
    {
        state.offPathTicks = 0;
        return false;
    }

    std::optional<G3D::Vector3> cur = PointAt(path, state.segmentIdx, state.pointIdx);
    if (!cur.has_value())
    {
        state.offPathTicks = 0;
        return false;
    }

    float const px = bot->GetPositionX();
    float const py = bot->GetPositionY();
    float dist2 = std::numeric_limits<float>::max();

    if (std::optional<G3D::Vector3> prev = PrevPoint(path, state.segmentIdx, state.pointIdx))
        dist2 = std::min(dist2, Dist2ToSegment2DSq(px, py, *prev, *cur));
    if (std::optional<G3D::Vector3> next = NextPoint(path, state.segmentIdx, state.pointIdx))
        dist2 = std::min(dist2, Dist2ToSegment2DSq(px, py, *cur, *next));
    if (dist2 == std::numeric_limits<float>::max())
    {
        // No prev/next — single-point path. Fall back to distance to point.
        float const dx = px - cur->x;
        float const dy = py - cur->y;
        dist2 = dx * dx + dy * dy;
    }

    bool const off = dist2 > OFF_PATH_THRESHOLD * OFF_PATH_THRESHOLD;
    if (off)
        ++state.offPathTicks;
    else
        state.offPathTicks = 0;
    return off;
}

namespace
{
    // Flattens (segIdx, pointIdx) into a single index across the path's
    // polyline. Used only for Resnap's window walk — segment boundaries
    // don't matter to "is this point close to the bot".
    struct FlatIndex
    {
        uint32 seg;
        uint32 pt;
    };

    bool StepFlatForward(ChunkedPathfinder::Result const& path, FlatIndex& idx)
    {
        if (idx.seg >= path.segments.size())
            return false;
        ++idx.pt;
        while (idx.seg < path.segments.size() && idx.pt >= path.segments[idx.seg].polyline.size())
        {
            ++idx.seg;
            idx.pt = 0;
        }
        return idx.seg < path.segments.size();
    }

    bool StepFlatBackward(ChunkedPathfinder::Result const& path, FlatIndex& idx)
    {
        if (idx.pt > 0)
        {
            --idx.pt;
            return true;
        }
        while (idx.seg > 0)
        {
            --idx.seg;
            size_t const sz = path.segments[idx.seg].polyline.size();
            if (sz > 0)
            {
                idx.pt = static_cast<uint32>(sz) - 1;
                return true;
            }
        }
        return false;
    }

    float Dist3DSq(float px, float py, float pz, G3D::Vector3 const& q)
    {
        float const dx = px - q.x;
        float const dy = py - q.y;
        float const dz = pz - q.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // Eye-height bump so floor-level polyline points don't false-fail LOS on
    // stairs/slopes. Mirrors StridedPathfinder's LOS_Z_BUMP.
    constexpr float RESNAP_LOS_Z_BUMP = 1.5f;

    // Can the bot see `p` in a straight static-VMAP line? Used to keep Resnap
    // from teleporting the follower's cursor onto a point that is physically
    // close but on the far side of a wall — the classic U-turn / S-crossing
    // failure, where the two arms of the bend come within RESNAP_RADIUS and a
    // naive nearest-point snap skips the entire corridor between them.
    bool BotCanSee(Player* bot, G3D::Vector3 const& p)
    {
        if (!bot)
            return false;
        Map const* map = bot->GetMap();
        if (!map)
            return true;  // no map data — don't block the resnap
        return map->isInLineOfSight(bot->GetPositionX(), bot->GetPositionY(),
                                    bot->GetPositionZ() + RESNAP_LOS_Z_BUMP,
                                    p.x, p.y, p.z + RESNAP_LOS_Z_BUMP,
                                    bot->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP,
                                    VMAP::ModelIgnoreFlags::Nothing);
    }
}

bool DungeonPathFollower::Resnap(Player* bot, ChunkedPathfinder::Result const& path, DungeonFollowerState& state)
{
    if (!bot || path.segments.empty() || state.segmentIdx >= path.segments.size())
        return false;

    float const px = bot->GetPositionX();
    float const py = bot->GetPositionY();
    float const pz = bot->GetPositionZ();

    // Walk a window of polyline points around the current cursor in both
    // directions; pick the one closest to the bot in 3D. Forward bias on
    // ties: among equally-close candidates, prefer the later one so the
    // resnap doesn't replay corridor we already cleared.
    float bestDist2 = std::numeric_limits<float>::max();
    uint32 bestSeg = state.segmentIdx;
    uint32 bestPoint = state.pointIdx;
    bool found = false;

    // Forward walk (starts at current point, includes it). Candidates the bot
    // can't see in a straight line are skipped — on a U-turn whose arms come
    // within RESNAP_RADIUS, a forward point on the far arm is physically near
    // but walled off; snapping to it would skip the whole bend and leave the
    // bot trying to cross the inside wall.
    FlatIndex fwd{state.segmentIdx, state.pointIdx};
    for (size_t step = 0; step <= RESNAP_WINDOW; ++step)
    {
        std::optional<G3D::Vector3> p = PointAt(path, fwd.seg, fwd.pt);
        if (p.has_value() && BotCanSee(bot, *p))
        {
            float const d2 = Dist3DSq(px, py, pz, *p);
            // <= so the *furthest forward* tie wins (forward bias).
            if (d2 <= bestDist2)
            {
                bestDist2 = d2;
                bestSeg = fwd.seg;
                bestPoint = fwd.pt;
                found = true;
            }
        }
        if (!StepFlatForward(path, fwd))
            break;
    }

    // Backward walk (starts one point before current; "<" so backward
    // candidates only win on strictly closer distance). Same LOS gate.
    FlatIndex back{state.segmentIdx, state.pointIdx};
    for (size_t step = 0; step < RESNAP_WINDOW; ++step)
    {
        if (!StepFlatBackward(path, back))
            break;
        std::optional<G3D::Vector3> p = PointAt(path, back.seg, back.pt);
        if (!p.has_value() || !BotCanSee(bot, *p))
            continue;
        float const d2 = Dist3DSq(px, py, pz, *p);
        if (d2 < bestDist2)
        {
            bestDist2 = d2;
            bestSeg = back.seg;
            bestPoint = back.pt;
            found = true;
        }
    }

    if (!found || bestDist2 > RESNAP_RADIUS * RESNAP_RADIUS)
        return false;

    state.segmentIdx = bestSeg;
    state.pointIdx = bestPoint;
    state.offPathTicks = 0;
    return true;
}

std::vector<G3D::Vector3> DungeonPathFollower::BuildSplineWindow(Player* bot,
    ChunkedPathfinder::Result const& path, DungeonFollowerState const& state)
{
    std::vector<G3D::Vector3> window;
    if (!bot || path.segments.empty() || state.segmentIdx >= path.segments.size())
        return window;

    // Element 0 is the live position. MoveSplineInit::Launch overwrites
    // path[0] with the unit's real position anyway, but seeding it keeps the
    // initial spline tangent correct and matches the escort path[0]=start
    // convention.
    window.push_back(G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));

    uint32 seg = state.segmentIdx;
    uint32 pt = state.pointIdx;
    while (seg < path.segments.size() && window.size() < MAX_SPLINE_WINDOW_POINTS)
    {
        PathSegment const& s = path.segments[seg];
        // A jump leg can't be expressed as a ground spline — deliver the bot
        // to the lip and let the per-hop JumpTo branch take the jump point.
        if (s.jumpDown || s.jumpGap)
            break;

        if (pt < s.polyline.size())
        {
            window.push_back(s.polyline[pt]);
            ++pt;
        }
        else
        {
            ++seg;
            pt = 0;
        }
    }

    return window;
}
