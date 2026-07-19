/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestRunLiveJson.h"

using DcTestRunLive::BotPos;
using DcTestRunLive::Build;
using DcTestRunLive::PlanSnapshot;
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
    EXPECT_EQ(json, "{\"active\":false,\"ts\":1700000000,\"plans\":[],\"runs\":[]}");
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

// ---- runs carry their owning plan ------------------------------------------------

TEST(DcTestRunLiveJsonTest, RunPlanIdEmittedAndEmptyForAdHoc)
{
    RunSnapshot planChild = Sample("tr-1", "deadmines");
    planChild.planId = "tp-9";
    RunSnapshot adHoc = Sample("tr-2", "wailing");

    std::string const json = Build(1700000000ull, {planChild, adHoc});
    EXPECT_NE(json.find("\"runId\":\"tr-1\",\"planId\":\"tp-9\""), std::string::npos);
    EXPECT_NE(json.find("\"runId\":\"tr-2\",\"planId\":\"\""), std::string::npos);
}

// ---- plans array -----------------------------------------------------------------

TEST(DcTestRunLiveJsonTest, PlansArrayCarriesProgressAndKeepsFileActive)
{
    PlanSnapshot p;
    p.planId = "tp-9";
    p.dungeon = "old-hillsbrad";
    p.total = 20;
    p.launched = 9;
    p.succeeded = 6;
    p.failed = 1;
    p.activeNow = 2;
    p.concurrent = 5;
    p.state = "running";
    p.elapsedS = 900;

    // A plan with zero runs in flight (backoff window) must still read active.
    std::string const json = Build(1700000000ull, {}, {p});
    EXPECT_NE(json.find("\"active\":true"), std::string::npos);
    EXPECT_NE(json.find("\"plans\":[{\"planId\":\"tp-9\",\"dungeon\":\"old-hillsbrad\""
                        ",\"total\":20,\"launched\":9,\"succeeded\":6,\"failed\":1"
                        ",\"active\":2,\"concurrent\":5,\"state\":\"running\""
                        ",\"elapsedS\":900}]"),
              std::string::npos);
    EXPECT_NE(json.find("\"runs\":[]"), std::string::npos);
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

// ---- no bots: empty array + mapId still emitted ----------------------------------

TEST(DcTestRunLiveJsonTest, NoBotsEmitsEmptyArrayAndDefaultMap)
{
    RunSnapshot s = Sample("tr-1", "deadmines");  // leaves mapId=-1, bots empty
    std::string const json = Build(1700000000ull, {s});
    EXPECT_NE(json.find("\"mapId\":-1"), std::string::npos);
    EXPECT_NE(json.find("\"bots\":[]"), std::string::npos);
}

// ---- bot positions: compact keys, 1-decimal coords, no timeline pollution --------

TEST(DcTestRunLiveJsonTest, BotPositionsAreCompactAndOutsideRecent)
{
    RunSnapshot s = Sample("tr-1", "ragefire");
    s.mapId = 389;
    s.bots.push_back(BotPos{"tank", 1, -123.456f, 42.0f, -60.19f, true});
    s.bots.push_back(BotPos{"healer", 5, 10.0f, -5.5f, 1.0f, false});

    std::string const json = Build(1700000000ull, {s});

    EXPECT_NE(json.find("\"mapId\":389"), std::string::npos);
    // Short keys + 1-decimal rounding (not the full float noise).
    EXPECT_NE(json.find("\"role\":\"tank\",\"cls\":1,\"x\":-123.5,\"y\":42.0,\"z\":-60.2,\"alive\":true"),
              std::string::npos);
    EXPECT_NE(json.find("\"role\":\"healer\",\"cls\":5,\"x\":10.0,\"y\":-5.5,\"z\":1.0,\"alive\":false"),
              std::string::npos);
    // Positions live in their own array, never spilled into the human-readable
    // "recent" timeline (which stays empty for this snapshot).
    EXPECT_NE(json.find("\"bots\":["), std::string::npos);
    EXPECT_NE(json.find("\"recent\":[]"), std::string::npos);
}
