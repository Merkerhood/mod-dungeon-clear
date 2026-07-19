/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTPLANSUMMARY_H
#define _PLAYERBOT_DCTESTPLANSUMMARY_H

#include <cstdint>
#include <string>
#include <vector>

#include "TestRun/DcTestPlan.h"

// Aggregates a finished plan's child-run outcomes into the one JSON line per
// plan appended to dc_testplans.jsonl (env override DC_TESTPLANS_FILE) — the
// dashboard's plan-history panel tails this file. Pure builder + the same
// append pattern as DcTestRunRecord.

namespace DcTestPlanSummary
{
    struct KeyCount
    {
        std::string key;
        std::uint32_t count = 0;
    };

    // Per-boss kill counts across the plan, in progression order (ascending
    // mean position in the runs' kill timelines) — the "where do runs die"
    // funnel. Only named mask-kills feed it (see DcTestPlan::RunOutcome).
    struct FunnelEntry
    {
        std::string name;
        std::uint32_t killed = 0;
    };

    struct Stats
    {
        std::uint32_t launched = 0;   // outcomes seen (== runs started)
        std::uint32_t succeeded = 0;
        std::uint32_t failed = 0;
        std::vector<KeyCount> verdicts;     // by count desc, then name
        std::vector<KeyCount> failReasons;  // failures only, by count desc
        // Duration stats over SUCCESSFUL runs only (failure durations are
        // timeout artifacts); all zero when no run succeeded.
        std::uint32_t minS = 0, avgS = 0, medianS = 0, maxS = 0;
        std::vector<FunnelEntry> funnel;
        std::vector<std::string> runIds;
    };

    Stats Build(std::vector<DcTestPlan::RunOutcome> const& outcomes);

    // Plan identity + disposition, carried alongside the aggregated stats.
    struct Header
    {
        std::uint32_t schema = 1;
        std::string planId;
        std::string dungeon;
        std::string dungeonName;
        std::uint32_t total = 0;
        std::uint32_t concurrent = 0;
        std::uint32_t level = 0;
        std::uint32_t seedBase = 0;
        std::uint64_t startedAtMs = 0;
        std::uint64_t endedAtMs = 0;
        std::uint32_t durationS = 0;
        std::string result;       // "completed" | "stopped" | "aborted"
        std::string abortReason;  // "" unless aborted
    };

    std::string ToJsonl(Header const& h, Stats const& s);

    std::string CapturePath();
    void Append(Header const& h, Stats const& s);
}

#endif  // _PLAYERBOT_DCTESTPLANSUMMARY_H
