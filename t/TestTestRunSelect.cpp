/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestRunSelect.h"

using DcTestRunSelect::Kind;
using DcTestRunSelect::Resolve;
using DcTestRunSelect::Result;
using DcTestRunSelect::RunRef;

namespace
{
    std::vector<RunRef> None() { return {}; }

    std::vector<RunRef> One()
    {
        return {{"tr-1", "deadmines"}};
    }

    // Two distinct dungeons plus a second run of the first dungeon, so the
    // dungeon-token path can resolve to more than one index.
    std::vector<RunRef> Three()
    {
        return {{"tr-1", "deadmines"}, {"tr-2", "wailing-caverns"}, {"tr-3", "deadmines"}};
    }
}

// ---- bare selector ---------------------------------------------------------------

TEST(DcTestRunSelectTest, EmptySelectorNoRuns)
{
    Result const r = Resolve("", None());
    EXPECT_EQ(r.kind, Kind::NoRuns);
    EXPECT_TRUE(r.indices.empty());
}

TEST(DcTestRunSelectTest, EmptySelectorOneRunPicksIt)
{
    Result const r = Resolve("", One());
    EXPECT_EQ(r.kind, Kind::All);
    ASSERT_EQ(r.indices.size(), 1u);
    EXPECT_EQ(r.indices[0], 0u);
}

TEST(DcTestRunSelectTest, EmptySelectorTwoRunsAmbiguous)
{
    Result const r = Resolve("", Three());
    EXPECT_EQ(r.kind, Kind::Ambiguous);
    EXPECT_TRUE(r.indices.empty());
}

// ---- "all" -----------------------------------------------------------------------

TEST(DcTestRunSelectTest, AllSelectsEveryRun)
{
    Result const r = Resolve("all", Three());
    EXPECT_EQ(r.kind, Kind::All);
    ASSERT_EQ(r.indices.size(), 3u);
    EXPECT_EQ(r.indices[0], 0u);
    EXPECT_EQ(r.indices[1], 1u);
    EXPECT_EQ(r.indices[2], 2u);
}

TEST(DcTestRunSelectTest, AllWithNoRunsIsNoRuns)
{
    Result const r = Resolve("all", None());
    EXPECT_EQ(r.kind, Kind::NoRuns);
}

// ---- runId -----------------------------------------------------------------------

TEST(DcTestRunSelectTest, RunIdExactMatch)
{
    Result const r = Resolve("tr-2", Three());
    EXPECT_EQ(r.kind, Kind::Matched);
    ASSERT_EQ(r.indices.size(), 1u);
    EXPECT_EQ(r.indices[0], 1u);
}

// ---- dungeon token ---------------------------------------------------------------

TEST(DcTestRunSelectTest, DungeonTokenSingleRun)
{
    Result const r = Resolve("wailing-caverns", Three());
    EXPECT_EQ(r.kind, Kind::Matched);
    ASSERT_EQ(r.indices.size(), 1u);
    EXPECT_EQ(r.indices[0], 1u);
}

TEST(DcTestRunSelectTest, DungeonTokenMultipleRuns)
{
    Result const r = Resolve("deadmines", Three());
    EXPECT_EQ(r.kind, Kind::Matched);
    ASSERT_EQ(r.indices.size(), 2u);
    EXPECT_EQ(r.indices[0], 0u);
    EXPECT_EQ(r.indices[1], 2u);
}

// ---- precedence: runId beats a same-string dungeon token -------------------------

TEST(DcTestRunSelectTest, RunIdBeatsDungeonToken)
{
    // A run whose dungeon token literally equals another run's runId. The
    // exact-runId pass must win, resolving to that one run only.
    std::vector<RunRef> runs = {{"tr-x", "boss"}, {"boss", "wailing-caverns"}};
    Result const r = Resolve("boss", runs);
    EXPECT_EQ(r.kind, Kind::Matched);
    ASSERT_EQ(r.indices.size(), 1u);
    EXPECT_EQ(r.indices[0], 1u);  // the run whose runId == "boss"
}

// ---- not found -------------------------------------------------------------------

TEST(DcTestRunSelectTest, UnknownSelectorNotFound)
{
    Result const r = Resolve("nope", Three());
    EXPECT_EQ(r.kind, Kind::NotFound);
    EXPECT_TRUE(r.indices.empty());
}
