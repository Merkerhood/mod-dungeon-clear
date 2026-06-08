/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARUTIL_H
#define _PLAYERBOT_DUNGEONCLEARUTIL_H

#include <optional>
#include <string>
#include <vector>

#include "Log.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
// Umbrella: units split out of the former DungeonClearUtil god-class. Files that
// include DungeonClearUtil.h keep resolving DcEngageGeometry::/DcTargeting::/...
// without per-file include churn during the decomposition.
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLootPolicy.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFollowerLifecycle.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPartyState.h"

// --- Advanced-pull log channel --------------------------------------------
// Pull mode (LOS pull-to-camp) gets its OWN log category, a child of the main
// DungeonClear channel (playerbots.dungeonclear.pull). It inherits the parent
// appender by default, but can be split to its own file and raised to Debug /
// Trace independently — without flooding the main channel — via
// Logger.playerbots.dungeonclear.pull in mod_dungeon_clear.conf. The feature is
// young and chatty to debug, so every pull state-machine transition, camp
// decision, leg watchdog, and passive-management step routes through these.
//
//   DC_PULL_INFO  — milestones a maintainer wants even at the default level
//                   (camp marked, aggro confirmed, party released, aborts).
//   DC_PULL_DEBUG — per-decision detail (distances, gate outcomes, phase math).
//   DC_PULL_TRACE — per-tick spam (run-in/return movement, hold-at-camp parking).
#define DC_PULL_LOG_CATEGORY "playerbots.dungeonclear.pull"
#define DC_PULL_INFO(...)  LOG_INFO(DC_PULL_LOG_CATEGORY, __VA_ARGS__)
#define DC_PULL_DEBUG(...) LOG_DEBUG(DC_PULL_LOG_CATEGORY, __VA_ARGS__)
#define DC_PULL_TRACE(...) LOG_TRACE(DC_PULL_LOG_CATEGORY, __VA_ARGS__)

class WorldObject;
class Player;
class Unit;
class Creature;
class GameObject;
class InstanceScript;
class PlayerbotAI;
class AiObjectContext;
struct DungeonBossInfo;
struct Position;

// DcPullPhase + DcPullContext now live in DcPullContext.h (included above) so the
// one owned pull-state struct is visible to both the util layer and the value
// layer without a circular include.

class DungeonClearUtil
{
public:
    // Returns the closest hostile, alive unit from `possibleTargets` that
    // sits within `range` of the bot AND within `halfAngle` radians of the
    // direction from bot to boss. Nullptr if none. Geometry-only — used as
    // the fallback when corridor computation isn't available.
    static Unit* FindBlockingTrash(Player* bot,
                                   DungeonBossInfo const& boss,
                                   float range,
                                   float halfAngle,
                                   GuidVector const& possibleTargets);


    // Returns the closest hostile, alive unit from `possibleTargets` whose
    // 2D distance to the path polyline is within its blocking band, considering
    // only the segment of the path within the first maxLookAhead yards from the
    // bot. LOS-checked. Nullptr if none. With DungeonClear.DynamicAggroRange on,
    // each candidate's band is its real aggro range (clamped to the trash
    // floor/cap); off, every candidate uses the fixed `corridorWidth`.
    static Unit* FindBlockingTrashCorridor(Player* bot,
                                           Movement::PointsArray const& corridor,
                                           float maxLookAhead,
                                           float corridorWidth,
                                           GuidVector const& possibleTargets);

    // The trash pack the advanced-pull maneuver should grab, or nullptr. Mirrors
    // the blocking-trash trigger's primary detection (corridor scan along the
    // cached long-path, falling back to the geometric cone) plus the closed-door
    // veto, so the pull aims at the same pack the normal flow would walk in to.
    // Shared by the pull trigger (decide to start) and the pull action (where to
    // run). Reads the bot's AI values via `botAI`.
    static Unit* FindPullTarget(PlayerbotAI* botAI, DungeonBossInfo const& next);

    // Returns a live spawned creature with the given entry on the bot's map, or
    // nullptr if none exists or all are dead.
    static Creature* FindLiveCreatureOnMap(Player* bot, uint32 entry);

    // Live boss creature for `entry`, resolved through the cached
    // "dungeon clear live boss" GUID (O(1) ObjectAccessor lookup) instead of
    // re-scanning the whole creature store on every call — the trigger ladder
    // and advance action ask for it several times per tick. Falls back to a
    // direct store scan when the cache was computed for a different boss (the
    // brief window just after a boss change) or the cached GUID went stale.
    // Returns nullptr when the boss isn't loaded/alive on the map.
    static Creature* GetLiveBoss(Player* bot, AiObjectContext* ctx, uint32 entry);

    // Returns true if at least one spawned creature with the given entry exists
    // on the bot's map (alive or dead). Distinguishes "missing" from "killed".
    static bool IsCreaturePresentOnMap(Player* bot, uint32 entry);

















    // LOOT_SKIP_STICKY now lives in DcLootPolicy.







    // Returns a per-bot "camp slot": the shared camp anchor nudged 1-2yd in a
    // deterministic, GUID-derived direction so a party holding at camp fans out
    // instead of stacking on one identical coordinate (the bot-like single-point
    // look). The offset is run through PathGenerator and the navmesh-snapped
    // endpoint is returned, so the slot is guaranteed to sit inside the zone
    // geometry; if the probe can't produce a real ground path near camp (camp
    // against a wall / on a ledge) it falls back to the exact camp position. The
    // result is stable across ticks for a given (bot, camp), so MoveTo dedups it
    // and the follower settles on one spot instead of jittering.
    static Position ComputeCampSlot(Player* bot, Position const& camp);

    // Scans `candidates` for the closest hostile alive unit whose 2D distance
    // to any segment of the supplied path polyline (within the first
    // `maxLookAhead` yards of forward travel) is within its blocking band.
    // LOS-checked. Nullptr if none. Replaces the single-segment
    // FindBlockingTrashCorridor for long routes that fan across multiple chunks.
    // With DungeonClear.DynamicAggroRange on the band is each candidate's real
    // aggro range (clamped to the trash floor/cap); off it is `corridorWidth`.
    static Unit* FindBlockingTrashOnPath(Player* bot,
                                         std::vector<PathSegment> const& segments,
                                         float maxLookAhead,
                                         float corridorWidth,
                                         GuidVector const& candidates);

    // Scans every loaded creature on the bot's map for an alive hostile that
    // (a) is not already in combat with someone else and (b) the bot can path
    // to. Returns the closest such unit, or nullptr. Used by the stalled
    // fallback to kill obstacles when no path to the boss exists.
    static Unit* FindNearestReachableHostile(Player* bot);

    // Returns the InstanceScript driving the bot's current map, or nullptr
    // if the map is not an instance map or has no script. Used by the
    // next-boss probe to consult authoritative encounter state before
    // falling back to spawn-store creature scanning.
    static InstanceScript* GetInstanceScript(Player* bot);




    // --- Dynamic pull (auto Leeroy vs Advanced) -----------------------------
    // Classifies the pull on `target`: true => use the careful Advanced pull-to-
    // camp; false => Leeroy it. Estimates how many mobs would aggro if the party
    // fought on top of the target — every mob whose own (level-scaled) aggro
    // radius + PullCombatSpread reaches the camp spot, plus one CallForHelp assist
    // hop — gated to mobs in line of sight, on the same floor, with no closed door
    // between (the far-targets scan ignores LOS). True when that estimate exceeds
    // PullDynamicMaxLeeroyMobs. Reach comes from the real creature aggro radius
    // (vs the lowest-level party member), so it self-tunes per zone/level. The pure
    // count is DungeonClearMath::EstimateAggroCount. See the Dynamic Pull plan doc.
    static bool ClassifyPullAdvanced(PlayerbotAI* botAI, Unit* target);

    // Per-tick governor for Dynamic mode (pull setting == 2). No-op for Off/On
    // (DcPullAction owns the bool there). Out of combat with no pull maneuver in
    // flight, it sizes up the next pull target (ClassifyPullAdvanced), latches the
    // verdict per target GUID (so a single approaching pack isn't re-judged every
    // tick and the party isn't churned between follow/hold), and drives `dungeon
    // clear pull mode` (+ leader daze immunity + camp seed) so the rest of the
    // existing pipeline runs the chosen maneuver. Called at the top of
    // DungeonClearPullTrigger::IsActive, which the engine evaluates before the
    // engage triggers each tick. Publishes the verdict to `dungeon clear pull
    // decision` for the addon. Leader-only (the caller has already gated on that).
    static void UpdateDynamicPullMode(PlayerbotAI* botAI, AiObjectContext* context);


    // Picks the advanced-pull camp: a rally point `setback` yards BACK along the
    // already-cleared route, where the tank drags the pulled pack and the party
    // waits. Dungeon mobs have no leash, so the camp is placed a generous fixed
    // distance back for room rather than the minimum that "works". If the point
    // `setback` back is still within `safeRadius` of another pack (any live
    // hostile that is not `target` and not one of `target`'s packmates) the search
    // keeps walking back, up to `maxDrag`, until clear. Cleared route behind the
    // tank is inherently safe ground (we already killed through it). When the
    // cleared route behind is too short (e.g. the first pull near the entrance) it
    // falls back to a straight line away from the target, snapped to the navmesh.
    // Returns nullopt only when there is no usable position at all (no bot/target).
    // The chosen point's clearance is written to `clearanceOut` (FLT_MAX when no
    // other pack exists) and how far back it sits to `dragOut`, for the plan log.
    static std::optional<Position> ComputeSafeCamp(PlayerbotAI* botAI, Unit* target,
                                                   float setback, float safeRadius,
                                                   float maxDrag,
                                                   float& clearanceOut, float& dragOut);

    // Lean, target-less twin of ComputeSafeCamp for the Idle SCOUT phase: returns
    // a point `setback` back along the breadcrumb trail behind the tank (capped at
    // maxDrag), so the camp can TRAIL the moving tank while no pull is committed
    // and the party walks along behind it at a fixed standoff. No clearance test —
    // the tank just walked this ground, so it is by definition clear, and there is
    // no pack to stay clear of yet. Falls back to the farthest contiguous trail
    // point, then to the tank's own position when the trail is too short. Returns
    // nullopt only when there is no bot.
    static std::optional<Position> ComputeTrailCamp(PlayerbotAI* botAI, float setback,
                                                    float maxDrag);

    // True when the party is "set" for the tank to pull: every living, on-map,
    // non-leader BOT follower is within `setRadius` of `camp` AND currently
    // running the combat-engine "passive" strategy (so it won't break the pull).
    // Real-player members (no PlayerbotAI) are not waited on. Solo (no group)
    // returns true. Lets the Forming gate hold the tag until the party has
    // actually parked and gone passive, instead of pulling into open ground.
    static bool IsPartySetAtCamp(Player* leader, Position const& camp, float setRadius);









};

#endif
