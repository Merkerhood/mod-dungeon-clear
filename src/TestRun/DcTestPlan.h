/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTPLAN_H
#define _PLAYERBOT_DCTESTPLAN_H

#include <cstdint>
#include <string>
#include <vector>

// Pure kernel for `.dc test plan` campaigns: a plan keeps up to `concurrent`
// test runs of one dungeon in flight until `total` have completed, then the
// outcomes are aggregated into one summary (DcTestPlanSummary). Engine-free —
// the launch decision and the argument parsing are unit-testable in isolation
// (mirroring DcTestRunVerdict / DcTestRunSelect); DcTestPlanManager owns the
// live state and feeds this kernel.

namespace DcTestPlan
{
    struct Spec
    {
        std::string planId;        // "tp-…", assigned by the manager
        std::string dungeonToken;  // DcTestDungeonRegistry token
        std::uint32_t total = 0;       // runs to complete (failures count)
        std::uint32_t concurrent = 0;  // plan-local in-flight cap
        std::uint32_t level = 0;       // 0 = registry recommendedLevel
        std::uint32_t seedBase = 0;    // 0 = random comp per run;
                                       // N = child i replays seed N+i
    };

    struct Counters
    {
        std::uint32_t launched = 0;   // Start() accepted (active + finished)
        std::uint32_t succeeded = 0;
        std::uint32_t failed = 0;     // any non-success incl. setup_failed
        std::uint32_t activeNow = 0;  // live child runs
    };

    // One finished child run, distilled from its DcTestRunRecord::Record by the
    // run manager's erase pass. bossKills keeps only named mask-kills (in kill
    // order) — anchor "objective" completions carry no name worth aggregating.
    struct RunOutcome
    {
        std::string runId;
        std::string result;      // verdict token ("success", "setup_failed", …)
        std::string failReason;
        std::uint32_t durationS = 0;
        std::uint32_t bossesKilled = 0;
        std::uint32_t bossesTotal = 0;
        std::vector<std::string> bossKills;
    };

    // How many new runs to launch this tick: bounded by the plan's remaining
    // budget (total - launched), the plan's own concurrency headroom, and the
    // run manager's global cap headroom; zero while a backoff is pending.
    std::uint32_t LaunchesWanted(Spec const& s, Counters const& c,
                                 std::uint32_t globalHeadroom, std::uint32_t backoffMs);

    // A plan started from the console has no issuing GM until the headless
    // test driver finishes logging in, so the scheduler has to sit on its
    // hands for the first few ticks. Wait while the login is in flight, but
    // bound it — a driver that can't come up at all (or one whose login never
    // lands) must fail the plan rather than leave it parked forever.
    enum class DriverWait
    {
        Wait,
        Abort,
    };
    inline DriverWait DriverWaitVerdict(bool loginPending, std::uint32_t waitedMs,
                                        std::uint32_t capMs)
    {
        if (!loginPending)
            return DriverWait::Abort;  // misconfigured — retrying can't fix it
        return waitedMs >= capMs ? DriverWait::Abort : DriverWait::Wait;
    }

    // A plan is finished (ready to summarize) once nothing is in flight and
    // either the total has completed or the plan was told to stop launching.
    inline bool IsFinished(Spec const& s, Counters const& c, bool stopping)
    {
        return c.activeNow == 0 && (stopping || c.succeeded + c.failed >= s.total);
    }

    // `.dc test plan start <token> total=N [concurrent=N] [level=N] [seed=N]`.
    // Fills token/total/concurrent/level/seedBase; planId is left empty. ok is
    // false with a usage-shaped err on a missing token/total, a duplicate bare
    // word, or a malformed key=value.
    struct ParseResult
    {
        bool ok = false;
        std::string err;
        Spec spec;
    };
    ParseResult ParseStartArgs(std::string const& args);
}

#endif  // _PLAYERBOT_DCTESTPLAN_H
