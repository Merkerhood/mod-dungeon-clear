/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTRUNVERDICT_H
#define _PLAYERBOT_DCTESTRUNVERDICT_H

#include <cstdint>

// Pure verdict kernel for the automated dungeon test harness (`.dc test`).
// Each monitoring tick the manager distills the live run into an Observation
// and asks this kernel whether the run is still going, has succeeded, or has
// failed — and why. Extracted engine-free so the precedence ladder is
// unit-testable in isolation, mirroring DcWaitAtBossDecision / DcSmartRest.
//
// Precedence (first match wins):
//   1. The run's own disable funnel fired (DisableDungeonClear) — that verdict
//      is authoritative: all-cleared reason = Success, anything else (party
//      death, dc off, left the dungeon) = FailDisabled.
//   2. Operator abort / leader gone / GM logged out — FailAborted.
//   3. Pause outlasting its grace — a paused run is waiting for human input,
//      which a test run by definition never gets. The grace period exists so
//      the door-blocked auto-resume can win the race before we call it.
//   4. Stall outlasting its grace — the stall ladder gets time to recover.
//   5. No boss/objective progress for too long — the silent-livelock net.
//   6. Overall wall-clock cap.

namespace DcTestRun
{
    struct Limits
    {
        std::uint32_t pauseGraceMs = 60 * 1000;
        std::uint32_t stallGraceMs = 120 * 1000;
        std::uint32_t noProgressMs = 600 * 1000;
        std::uint32_t overallTimeoutMs = 3600 * 1000;
    };

    enum class Verdict : std::uint8_t
    {
        Continue,
        Success,
        FailDisabled,        // disable fired with a non-all-cleared reason
        FailPausedTimeout,
        FailStalledTimeout,
        FailNoProgress,
        FailOverallTimeout,
        FailAborted          // .dc test stop, GM logout, leader/bot missing
    };

    struct Observation
    {
        bool disableFired = false;
        bool disableAllCleared = false;
        bool abortRequested = false;
        bool leaderMissing = false;
        bool gmOnline = true;
        bool paused = false;
        bool stalled = false;
        std::uint32_t pausedForMs = 0;
        std::uint32_t stalledForMs = 0;
        std::uint32_t sinceProgressMs = 0;
        std::uint32_t elapsedMs = 0;
    };

    Verdict Classify(Observation const& o, Limits const& l);

    // Stable machine token for the JSONL record ("success", "paused_timeout",
    // ...). "continue" is never written; it means keep monitoring.
    char const* VerdictName(Verdict v);

    inline bool IsTerminal(Verdict v) { return v != Verdict::Continue; }
    inline bool IsSuccess(Verdict v) { return v == Verdict::Success; }
}

#endif  // _PLAYERBOT_DCTESTRUNVERDICT_H
