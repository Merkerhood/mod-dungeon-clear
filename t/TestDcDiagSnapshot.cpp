/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <sstream>

#include "TestRun/DcDiagSnapshot.h"

// Capture() needs a live bot and is exercised in-game; these pin the
// SERIALIZER, which is what the dashboard and any post-mortem actually parse.
// A malformed diag object would corrupt the whole JSONL line it is embedded in,
// taking the rest of the run history down with it.

using DcDiag::BossSnapshot;
using DcDiag::MemberSnapshot;
using DcDiag::Snapshot;

namespace
{
    std::string Json(Snapshot const& snap)
    {
        std::ostringstream s;
        s.precision(9);
        DcDiag::AppendJson(s, snap);
        return s.str();
    }

    Snapshot Sample()
    {
        Snapshot snap;
        snap.valid = true;
        snap.capturedAt = "teardown";
        snap.enabled = true;
        snap.stateStr = "stalled";
        snap.phase = "moving";
        snap.stallReason = "can't reach Mutanus the Devourer";
        snap.nextBossEntry = 3654;
        snap.nextBossName = "Mutanus the Devourer";
        snap.stickyBoss = 3654;
        snap.approachTargetEntry = 3654;
        snap.distToTarget = 84.5f;
        snap.mapId = 43;
        snap.partySize = 5;
        snap.aliveCount = 4;
        return snap;
    }
}

// ---- an unresolvable tank still produces a parseable object ----------------

TEST(DcDiagSnapshotTest, InvalidSnapshotClosesTheObject)
{
    Snapshot snap;  // valid == false
    snap.capturedAt = "teardown";

    std::string const json = Json(snap);

    EXPECT_EQ(json, "{\"valid\":false,\"capturedAt\":\"teardown\"}");
}

// ---- the fields a post-mortem actually reads -------------------------------

TEST(DcDiagSnapshotTest, CarriesStallStateAndTarget)
{
    std::string const json = Json(Sample());

    EXPECT_NE(json.find("\"valid\":true"), std::string::npos);
    EXPECT_NE(json.find("\"state\":\"stalled\""), std::string::npos);
    EXPECT_NE(json.find("\"phase\":\"moving\""), std::string::npos);
    EXPECT_NE(json.find("\"stallReason\":\"can't reach Mutanus the Devourer\""),
              std::string::npos);
    EXPECT_NE(json.find("\"nextName\":\"Mutanus the Devourer\""), std::string::npos);
    EXPECT_NE(json.find("\"sticky\":3654"), std::string::npos);
}

// ---- the three target notions disagreeing is called out explicitly ---------

TEST(DcDiagSnapshotTest, TargetMismatchIsSerialized)
{
    Snapshot snap = Sample();
    snap.targetMismatch = true;

    EXPECT_NE(Json(snap).find("\"mismatch\":true"), std::string::npos);
}

// ---- party rows: dead members and off-map members are distinguishable ------

TEST(DcDiagSnapshotTest, PartyRowsCarryLivenessAndDistance)
{
    Snapshot snap = Sample();

    MemberSnapshot dead;
    dead.name = "Healbot";
    dead.online = true;
    dead.alive = false;
    dead.healthPct = 0;
    dead.distToTank = 31.2f;
    snap.members.push_back(dead);

    // Left behind outside the instance: distance to the tank is meaningless
    // and reported as -1 rather than a misleading huge number.
    MemberSnapshot offMap;
    offMap.name = "Straggler";
    offMap.online = true;
    offMap.alive = true;
    offMap.mapId = 0;
    offMap.distToTank = -1.f;
    snap.members.push_back(offMap);

    std::string const json = Json(snap);

    EXPECT_NE(json.find("\"name\":\"Healbot\""), std::string::npos);
    EXPECT_NE(json.find("\"alive\":false"), std::string::npos);
    EXPECT_NE(json.find("\"distToTank\":-1"), std::string::npos);
    EXPECT_NE(json.find("\"size\":5"), std::string::npos);
    EXPECT_NE(json.find("\"alive\":4"), std::string::npos);
}

// ---- doneVia is the field that explains a no_progress verdict --------------
//
// The no-progress watchdog only resets on a "mask" or "anchor" completion. A
// roster whose kills all report "bossState" is a run that was clearing fine
// while the watchdog saw zero progress — so the token must survive into JSON.

TEST(DcDiagSnapshotTest, RosterRecordsWhichCompletionPathFired)
{
    Snapshot snap = Sample();

    BossSnapshot mask;
    mask.entry = 100;
    mask.name = "Rhahk'Zor";
    mask.kind = "boss";
    mask.status = "dead";
    mask.doneVia = "mask";
    snap.roster.push_back(mask);

    BossSnapshot gate;
    gate.entry = 200;
    gate.name = "Gatewatcher";
    gate.kind = "boss";
    gate.status = "dead";
    gate.doneVia = "bossState";
    snap.roster.push_back(gate);

    BossSnapshot pending;
    pending.entry = 300;
    pending.name = "Mutanus the Devourer";
    pending.kind = "boss";
    pending.status = "alive";
    pending.isTarget = true;
    snap.roster.push_back(pending);

    std::string const json = Json(snap);

    EXPECT_NE(json.find("\"doneVia\":\"mask\""), std::string::npos);
    EXPECT_NE(json.find("\"doneVia\":\"bossState\""), std::string::npos);
    EXPECT_NE(json.find("\"doneVia\":\"\""), std::string::npos);
    EXPECT_NE(json.find("\"isTarget\":true"), std::string::npos);
}

// ---- free text must not break the enclosing JSONL line ---------------------

TEST(DcDiagSnapshotTest, EscapesQuotesAndNewlines)
{
    Snapshot snap = Sample();
    snap.stallReason = "said \"stuck\"\nand gave up";
    snap.pathFailureReason = "no poly at\ttarget";

    std::string const json = Json(snap);

    EXPECT_NE(json.find("\\\"stuck\\\""), std::string::npos);
    EXPECT_NE(json.find("\\n"), std::string::npos);
    EXPECT_EQ(json.find('\n'), std::string::npos);   // no raw newline leaked
    EXPECT_EQ(json.find('\t'), std::string::npos);   // nor a raw tab
}

// ---- the log one-liner names the wedge ------------------------------------

TEST(DcDiagSnapshotTest, SummarizeNamesTheWedge)
{
    Snapshot snap = Sample();
    snap.doorStalled = true;
    snap.doorStalledForMs = 45000;
    snap.pathReachable = false;

    std::string const line = DcDiag::Summarize(snap);

    EXPECT_NE(line.find("state=stalled"), std::string::npos);
    EXPECT_NE(line.find("UNREACHABLE"), std::string::npos);
    EXPECT_NE(line.find("DOOR-STALLED 45s"), std::string::npos);
}

TEST(DcDiagSnapshotTest, SummarizeHandlesInvalidSnapshot)
{
    Snapshot snap;
    EXPECT_NE(DcDiag::Summarize(snap).find("unresolvable"), std::string::npos);
}
