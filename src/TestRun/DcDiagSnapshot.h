/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCDIAGSNAPSHOT_H
#define _PLAYERBOT_DCDIAGSNAPSHOT_H

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

class Player;

// A full read-only picture of a dungeon-clear run at one instant: where every
// party member is, what they are doing, which boss the run is on, how the route
// to it looks, and every watchdog counter that could explain a wedge.
//
// Written for the test-run harness's failure path, where a verdict token
// ("no_progress") on its own is not enough to diagnose anything: the run ends,
// the party is disbanded and logged out, and every bit of state that would have
// explained the failure is gone. Capture() must therefore be called BEFORE
// teardown disbands the group.
//
// Capture() is strictly read-only — it never mutates run state, so it is safe
// to call from a watchdog tick as well as from teardown. Note in particular
// that DungeonPathFollower::IsOffPath mutates offPathTicks and is deliberately
// NOT used here; route deviation comes from the pure RouteDeviation helper.
namespace DcDiag
{
    // One party member as seen at capture time. Positions are world coords in
    // the member's own map — a member on a different mapId than the tank has
    // been left behind outside the instance (or is mid-teleport).
    struct MemberSnapshot
    {
        std::string name;
        std::string className;
        std::uint64_t guid = 0;
        std::uint32_t level = 0;
        bool isBot = true;
        bool online = false;      // false => resolved from the group's member slots only
        std::uint32_t mapId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        float distToTank = 0.f;   // -1 when on a different map (distance is meaningless)
        bool alive = false;
        std::uint32_t healthPct = 0;
        std::uint32_t manaPct = 0;   // 0 for non-mana classes
        bool inCombat = false;
        std::string victim;       // name of GetVictim(), "" when not fighting
        bool dcStrategy = false;      // has the "dungeon clear" strategy
        bool dcCombatStrategy = false;
    };

    // One roster anchor (boss or objective) with the completion verdict derived
    // exactly as NextDungeonBossValue derives it — all three completion paths,
    // not just the DBC kill-bit.
    struct BossSnapshot
    {
        std::uint32_t entry = 0;
        std::uint32_t orderKey = 0;
        std::string name;
        std::string kind;         // "boss" | "objective"
        std::string status;       // "dead" | "skipped" | "alive" | "missing"
        std::string doneVia;      // "" | "mask" | "bossState" | "anchor" — WHICH path
        std::int32_t encounterIndex = -1;
        float x = 0.f, y = 0.f, z = 0.f;
        bool isTarget = false;    // the run's current NextDungeonBoss
        bool isSticky = false;    // the committed StickyBoss
    };

    struct Snapshot
    {
        bool valid = false;       // false => the tank could not be resolved
        std::string capturedAt;   // "teardown" | "sample"

        // --- run switches -------------------------------------------------
        bool enabled = false;
        bool paused = false;
        std::string pauseReason;
        bool pausedAtDoor = false;
        std::uint32_t selectedBossEntry = 0;
        bool smartRestLatched = false;

        // --- state-machine position ---------------------------------------
        std::string phase;        // raw DcKey::Phase token
        std::string stateStr;     // publisher-synthesized state
        std::string detail;       // publisher-synthesized human sentence
        std::string stallReason;  // THE field — why the run says it is stuck

        // --- target -------------------------------------------------------
        std::uint32_t stickyBoss = 0;
        std::uint32_t nextBossEntry = 0;
        std::string nextBossName;
        std::uint32_t committedTargetEntry = 0;  // approach's committed boss
        std::uint32_t approachTargetEntry = 0;   // boss the BUILT ROUTE leads to
        float distToTarget = -1.f;
        bool targetMismatch = false;  // the above disagree — usually IS the bug

        // --- route --------------------------------------------------------
        bool pathReachable = false;
        bool pathComplete = false;
        bool pathStartFarFromPoly = false;
        std::string pathFailureReason;
        std::uint32_t pathSegments = 0;
        std::uint32_t segmentIdx = 0;
        std::uint32_t pointIdx = 0;
        std::uint32_t offPathTicks = 0;
        float routeDeviation = -1.f;   // -1 = not measurable, NOT "on the corridor"
        bool cursorPastPathEnd = false;  // route consumed but target not reached

        // --- wedge watchdogs ----------------------------------------------
        std::uint32_t routeGlideStuck = 0;
        std::uint32_t doorWalkInStuck = 0;
        std::uint32_t pursuitStuck = 0;
        std::uint32_t finalApproachStuck = 0;
        std::uint32_t stuckCount = 0;
        std::uint32_t rebuildAttempts = 0;
        std::uint32_t resnapAttempts = 0;
        std::uint32_t partyNotReadyTicks = 0;

        // --- door ---------------------------------------------------------
        bool doorStalled = false;
        std::uint32_t doorStalledForMs = 0;

        // --- pull ---------------------------------------------------------
        std::uint32_t pullSetting = 0;
        std::uint32_t pullPhase = 0;
        std::uint32_t pullDecision = 0;
        std::uint32_t pullPhaseForMs = 0;
        std::uint32_t pullFizzleCount = 0;
        bool pullHasCamp = false;

        // --- world / party ------------------------------------------------
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        float tankX = 0.f, tankY = 0.f, tankZ = 0.f;
        bool tankInCombat = false;
        bool tankMoving = false;
        std::string tankVictim;
        std::uint32_t completedEncounterMask = 0;
        std::uint32_t partySize = 0;
        std::uint32_t aliveCount = 0;    // of the ONLINE members only
        std::uint32_t offlineCount = 0;  // in the group but not in the world
        std::uint32_t inCombatCount = 0;
        std::uint32_t clearedAnchors = 0;
        std::uint32_t skippedCount = 0;

        std::vector<MemberSnapshot> members;
        std::vector<BossSnapshot> roster;
    };

    // Read every field above off the leader tank. Safe on a null/despawned
    // tank (returns valid == false) and on a solo bot with no group.
    Snapshot Capture(Player* tank, char const* capturedAt);

    // Serialize as a JSON object (no trailing comma, no key) onto the stream,
    // matching DcTestRunRecord's hand-rolled JSONL style.
    void AppendJson(std::ostringstream& s, Snapshot const& snap);

    // One-line human summary for the worldserver log — the fields that most
    // often identify a wedge, in a form that greps well.
    std::string Summarize(Snapshot const& snap);
}

#endif  // _PLAYERBOT_DCDIAGSNAPSHOT_H
