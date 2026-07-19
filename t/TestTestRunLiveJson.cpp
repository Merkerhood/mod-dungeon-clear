/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestRunLiveJson.h"

using DcTestRunLive::Build;
using DcTestRunLive::RunSnapshot;
using DcTestRunLive::StatusEntry;

namespace
{
    RunSnapshot Sample(std::string const& runId, std::string const& dungeon)
    {
        RunSnapshot s;
        s.runId = runId;
        s.dungeon = dungeon;
        s.dungeonName = "The " + dungeon;
        s.stage = "monitoring";
        s.state = "clearing";
        s.level = 20;
        s.elapsedS = 42;
        s.bossesKilled = 1;
        s.bossesTotal = 4;
        return s;
    }
}

// ---- empty registry --------------------------------------------------------------

TEST(DcTestRunLiveJsonTest, EmptyIsInactiveWithEmptyRuns)
{
    std::string const json = Build(1700000000ull, {});
    EXPECT_EQ(json, "{\"active\":false,\"ts\":1700000000,\"runs\":[]}");
}

// ---- ts passthrough --------------------------------------------------------------

TEST(DcTestRunLiveJsonTest, TimestampIsVerbatim)
{
    std::string const json = Build(1234567890ull, {});
    EXPECT_NE(json.find("\"ts\":1234567890"), std::string::npos);
}

// ---- ordering + active flag ------------------------------------------------------

TEST(DcTestRunLiveJsonTest, TwoRunsPreserveOrderAndAreActive)
{
    std::vector<RunSnapshot> runs = {Sample("tr-1", "deadmines"), Sample("tr-2", "wailing")};
    std::string const json = Build(1700000000ull, runs);

    EXPECT_NE(json.find("\"active\":true"), std::string::npos);

    std::size_t const first = json.find("tr-1");
    std::size_t const second = json.find("tr-2");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_LT(first, second);

    // Per-run keys present.
    EXPECT_NE(json.find("\"dungeon\":\"deadmines\""), std::string::npos);
    EXPECT_NE(json.find("\"bossesKilled\":1"), std::string::npos);
    EXPECT_NE(json.find("\"bossesTotal\":4"), std::string::npos);
}

// ---- recent entries are escaped --------------------------------------------------

TEST(DcTestRunLiveJsonTest, RecentEntriesAreJsonEscaped)
{
    RunSnapshot s = Sample("tr-1", "deadmines");
    StatusEntry e;
    e.t = 10;
    e.state = "stalled";
    e.detail = "can't reach \"Boss\"\nretrying";  // quote + newline must escape
    s.recent.push_back(e);

    std::string const json = Build(1700000000ull, {s});
    EXPECT_NE(json.find("\\\"Boss\\\""), std::string::npos);
    EXPECT_NE(json.find("\\n"), std::string::npos);
    EXPECT_EQ(json.find('\n'), std::string::npos);  // no raw newline leaked
    EXPECT_NE(json.find("\"t\":10"), std::string::npos);
}
