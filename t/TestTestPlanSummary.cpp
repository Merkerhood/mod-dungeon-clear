/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestPlanSummary.h"

using DcTestPlan::RunOutcome;
using DcTestPlanSummary::Build;
using DcTestPlanSummary::Header;
using DcTestPlanSummary::Stats;
using DcTestPlanSummary::ToJsonl;

namespace
{
    RunOutcome Success(std::string const& runId, std::uint32_t durationS,
                       std::vector<std::string> bossKills = {})
    {
        RunOutcome o;
        o.runId = runId;
        o.result = "success";
        o.durationS = durationS;
        o.bossKills = std::move(bossKills);
        o.bossesKilled = static_cast<std::uint32_t>(o.bossKills.size());
        o.bossesTotal = 4;
        return o;
    }

    RunOutcome Failure(std::string const& runId, std::string const& result,
                       std::string const& reason, std::vector<std::string> bossKills = {})
    {
        RunOutcome o;
        o.runId = runId;
        o.result = result;
        o.failReason = reason;
        o.durationS = 3600;
        o.bossKills = std::move(bossKills);
        o.bossesKilled = static_cast<std::uint32_t>(o.bossKills.size());
        o.bossesTotal = 4;
        return o;
    }
}

// ---- counts + verdict histogram --------------------------------------------------

TEST(DcTestPlanSummaryTest, CountsAndVerdictHistogram)
{
    Stats const s = Build({Success("r1", 100), Success("r2", 200),
                           Failure("r3", "no_progress", "no boss progress"),
                           Failure("r4", "no_progress", "no boss progress"),
                           Failure("r5", "setup_failed", "bots did not log in")});
    EXPECT_EQ(s.launched, 5u);
    EXPECT_EQ(s.succeeded, 2u);
    EXPECT_EQ(s.failed, 3u);

    ASSERT_EQ(s.verdicts.size(), 3u);
    // Sorted by count desc; the 2-2 tie stays alphabetical (deterministic).
    EXPECT_EQ(s.verdicts[0].key, "no_progress");
    EXPECT_EQ(s.verdicts[0].count, 2u);
    EXPECT_EQ(s.verdicts[1].key, "success");
    EXPECT_EQ(s.verdicts[1].count, 2u);
    EXPECT_EQ(s.verdicts[2].key, "setup_failed");
    EXPECT_EQ(s.verdicts[2].count, 1u);

    ASSERT_EQ(s.failReasons.size(), 2u);
    EXPECT_EQ(s.failReasons[0].key, "no boss progress");
    EXPECT_EQ(s.failReasons[0].count, 2u);

    ASSERT_EQ(s.runIds.size(), 5u);
    EXPECT_EQ(s.runIds[0], "r1");
    EXPECT_EQ(s.runIds[4], "r5");
}

// ---- duration stats (successes only) ---------------------------------------------

TEST(DcTestPlanSummaryTest, DurationStatsUseSuccessesOnly)
{
    // Failure durations (3600s timeouts) must not pollute the stats.
    Stats const s = Build({Success("r1", 300), Success("r2", 100), Success("r3", 200),
                           Failure("r4", "overall_timeout", "exceeded limit")});
    EXPECT_EQ(s.minS, 100u);
    EXPECT_EQ(s.maxS, 300u);
    EXPECT_EQ(s.avgS, 200u);
    EXPECT_EQ(s.medianS, 200u);  // odd count: middle element
}

TEST(DcTestPlanSummaryTest, MedianOfEvenCountAverages)
{
    Stats const s = Build({Success("r1", 100), Success("r2", 200),
                           Success("r3", 300), Success("r4", 400)});
    EXPECT_EQ(s.medianS, 250u);
}

TEST(DcTestPlanSummaryTest, AllFailedLeavesDurationZero)
{
    Stats const s = Build({Failure("r1", "aborted", "stopped")});
    EXPECT_EQ(s.minS, 0u);
    EXPECT_EQ(s.avgS, 0u);
    EXPECT_EQ(s.medianS, 0u);
    EXPECT_EQ(s.maxS, 0u);
}

// ---- boss funnel -----------------------------------------------------------------

TEST(DcTestPlanSummaryTest, FunnelOrdersByProgressionAndCountsKills)
{
    // Two full clears, one death at the second boss: funnel must read
    // Drake(3) -> Skarloc(2) -> Epoch(2) in progression order even though the
    // map iteration order of the names is alphabetical.
    Stats const s = Build({Success("r1", 100, {"Lieutenant Drake", "Captain Skarloc", "Epoch Hunter"}),
                           Success("r2", 110, {"Lieutenant Drake", "Captain Skarloc", "Epoch Hunter"}),
                           Failure("r3", "no_progress", "wipe", {"Lieutenant Drake"})});
    ASSERT_EQ(s.funnel.size(), 3u);
    EXPECT_EQ(s.funnel[0].name, "Lieutenant Drake");
    EXPECT_EQ(s.funnel[0].killed, 3u);
    EXPECT_EQ(s.funnel[1].name, "Captain Skarloc");
    EXPECT_EQ(s.funnel[1].killed, 2u);
    EXPECT_EQ(s.funnel[2].name, "Epoch Hunter");
    EXPECT_EQ(s.funnel[2].killed, 2u);
}

TEST(DcTestPlanSummaryTest, FunnelDedupesRepeatKillsWithinARun)
{
    // A re-fired encounter bit must not double-count a run's kill.
    Stats const s = Build({Success("r1", 100, {"Boss A", "Boss A", "Boss B"})});
    ASSERT_EQ(s.funnel.size(), 2u);
    EXPECT_EQ(s.funnel[0].name, "Boss A");
    EXPECT_EQ(s.funnel[0].killed, 1u);
    EXPECT_EQ(s.funnel[1].name, "Boss B");
    EXPECT_EQ(s.funnel[1].killed, 1u);
}

// ---- empty plan ------------------------------------------------------------------

TEST(DcTestPlanSummaryTest, EmptyOutcomesProduceEmptyStats)
{
    Stats const s = Build({});
    EXPECT_EQ(s.launched, 0u);
    EXPECT_TRUE(s.verdicts.empty());
    EXPECT_TRUE(s.funnel.empty());
    EXPECT_TRUE(s.runIds.empty());
}

// ---- JSONL shape -----------------------------------------------------------------

TEST(DcTestPlanSummaryTest, ToJsonlCarriesHeaderAndStats)
{
    Header h;
    h.planId = "tp-1";
    h.dungeon = "old-hillsbrad";
    h.dungeonName = "Old Hillsbrad Foothills";
    h.total = 20;
    h.concurrent = 5;
    h.level = 68;
    h.seedBase = 7;
    h.startedAtMs = 1000;
    h.endedAtMs = 61000;
    h.durationS = 60;
    h.result = "completed";

    Stats const s = Build({Success("r1", 100, {"Lieutenant Drake"}),
                           Failure("r2", "no_progress", "he said \"no\"")});
    std::string const line = ToJsonl(h, s);

    EXPECT_NE(line.find("\"schema\":1"), std::string::npos);
    EXPECT_NE(line.find("\"planId\":\"tp-1\""), std::string::npos);
    EXPECT_NE(line.find("\"requested\":{\"total\":20,\"concurrent\":5,\"level\":68,\"seedBase\":7}"),
              std::string::npos);
    EXPECT_NE(line.find("\"result\":\"completed\""), std::string::npos);
    EXPECT_NE(line.find("\"runs\":{\"launched\":2,\"succeeded\":1,\"failed\":1}"),
              std::string::npos);
    EXPECT_NE(line.find("\"verdicts\":{"), std::string::npos);
    EXPECT_NE(line.find("\"success\":1"), std::string::npos);
    EXPECT_NE(line.find("\"bossFunnel\":[{\"name\":\"Lieutenant Drake\",\"killed\":1}]"),
              std::string::npos);
    EXPECT_NE(line.find("\"runIds\":[\"r1\",\"r2\"]"), std::string::npos);
    // Free-text reason is escaped, and no raw newline leaks into the line.
    EXPECT_NE(line.find("\\\"no\\\""), std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
}
