/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "DungeonClearApproach.h"

using DungeonClearApproach::DecideApproach;
using DungeonClearApproach::Observation;
using V = DungeonClearApproach::Verdict;

namespace
{
    // The action's real DC_* constants, passed into the pure function as inputs.
    constexpr uint32_t STUCK_TICK_LIMIT       = 5;   // DC_STUCK_TICK_LIMIT
    constexpr uint32_t PURSUIT_FAIL_LIMIT     = 5;   // DC_PURSUIT_FAIL_LIMIT
    constexpr uint32_t DONE_NOT_ENGAGED_LIMIT = 15;  // DC_DONE_NOT_ENGAGED_LIMIT

    // A healthy, on-corridor tick: reachable path, on the line, a spline window
    // ready to launch. Every guard above IssueSplineWindow is inactive, so the
    // baseline verdict is IssueSplineWindow. Tests flip one axis at a time.
    Observation Healthy()
    {
        Observation o;
        o.engageDist          = 100.0f;
        o.engageRange         = 22.0f;
        o.posStuckTicks       = 0;
        o.canPursue           = false;
        o.pursuitFailTicks    = 0;
        o.allowRecoveryMoves  = true;
        o.pathReachable       = true;
        o.asyncPending        = false;
        o.startFarFromPoly    = false;
        o.waterBetween        = false;
        o.offPath             = false;
        o.hopDone             = false;
        o.hopIsJump           = false;
        o.doneNotEngagedTicks = 0;
        o.splineRunning       = false;
        o.offLine             = false;
        o.haveSplineWindow    = true;
        o.stuckTickLimit      = STUCK_TICK_LIMIT;
        o.pursuitFailLimit    = PURSUIT_FAIL_LIMIT;
        o.doneNotEngagedLimit = DONE_NOT_ENGAGED_LIMIT;
        return o;
    }
}

// ---- Baseline / terminal -------------------------------------------------

TEST(DungeonClearApproachTest, HealthyTickIssuesSplineWindow)
{
    EXPECT_EQ(DecideApproach(Healthy()), V::IssueSplineWindow);
}

TEST(DungeonClearApproachTest, NoSplineWindowFallsToMoveTo)
{
    Observation o = Healthy();
    o.haveSplineWindow = false;
    EXPECT_EQ(DecideApproach(o), V::MoveToFallback);
}

// ---- posStuck threshold edge --------------------------------------------

TEST(DungeonClearApproachTest, PosStuckJustUnderLimitDoesNotRecover)
{
    Observation o = Healthy();
    o.posStuckTicks = STUCK_TICK_LIMIT - 1;  // 4
    EXPECT_EQ(DecideApproach(o), V::IssueSplineWindow);
}

TEST(DungeonClearApproachTest, PosStuckAtLimitRecovers)
{
    Observation o = Healthy();
    o.posStuckTicks = STUCK_TICK_LIMIT;  // 5
    EXPECT_EQ(DecideApproach(o), V::StuckRecover);
}

TEST(DungeonClearApproachTest, PosStuckOverLimitRecovers)
{
    Observation o = Healthy();
    o.posStuckTicks = STUCK_TICK_LIMIT + 3;
    EXPECT_EQ(DecideApproach(o), V::StuckRecover);
}

TEST(DungeonClearApproachTest, StuckRecoverOutranksEverything)
{
    // Even with a pursuable boss and an unreachable path, stuck wins (it is the
    // first rung).
    Observation o = Healthy();
    o.posStuckTicks = STUCK_TICK_LIMIT;
    o.canPursue = true;
    o.pathReachable = false;
    o.hopDone = true;
    EXPECT_EQ(DecideApproach(o), V::StuckRecover);
}

// ---- direct-pursuit latch boundary --------------------------------------

TEST(DungeonClearApproachTest, PursueWhenCanPursueAndLatchOpen)
{
    Observation o = Healthy();
    o.canPursue = true;
    o.pursuitFailTicks = PURSUIT_FAIL_LIMIT - 1;  // 4 < 5
    EXPECT_EQ(DecideApproach(o), V::Pursue);
}

TEST(DungeonClearApproachTest, PursuitLatchAtLimitStopsPursuing)
{
    // pursuitFailTicks == limit: the latch has tripped, so pursuit stands down
    // and the path machinery takes over (here, a healthy window).
    Observation o = Healthy();
    o.canPursue = true;
    o.pursuitFailTicks = PURSUIT_FAIL_LIMIT;  // 5, not < 5
    EXPECT_EQ(DecideApproach(o), V::IssueSplineWindow);
}

TEST(DungeonClearApproachTest, NoPursueWhenBossNotPursuable)
{
    Observation o = Healthy();
    o.canPursue = false;
    o.pursuitFailTicks = 0;
    EXPECT_EQ(DecideApproach(o), V::IssueSplineWindow);
}

TEST(DungeonClearApproachTest, PursueOutranksUnreachablePath)
{
    // Direct pursuit is decided before the long-path is even consulted.
    Observation o = Healthy();
    o.canPursue = true;
    o.pathReachable = false;
    o.asyncPending = true;
    EXPECT_EQ(DecideApproach(o), V::Pursue);
}

// ---- unreachable: async-pending vs. genuine failure ---------------------

TEST(DungeonClearApproachTest, UnreachableWithAsyncPendingWaits)
{
    Observation o = Healthy();
    o.pathReachable = false;
    o.asyncPending = true;
    o.startFarFromPoly = true;  // async wait outranks the off-mesh nudge
    o.waterBetween = true;
    EXPECT_EQ(DecideApproach(o), V::PlanRouteWait);
}

TEST(DungeonClearApproachTest, UnreachableFarFromPolyNudgesWhenAllowed)
{
    Observation o = Healthy();
    o.pathReachable = false;
    o.asyncPending = false;
    o.startFarFromPoly = true;
    o.allowRecoveryMoves = true;
    o.waterBetween = true;  // far-from-poly outranks swim
    EXPECT_EQ(DecideApproach(o), V::FarFromPolyRecover);
}

TEST(DungeonClearApproachTest, UnreachableFarFromPolyIgnoredWhenRecoveryDisabled)
{
    Observation o = Healthy();
    o.pathReachable = false;
    o.startFarFromPoly = true;
    o.allowRecoveryMoves = false;  // shim off -> falls through
    o.waterBetween = true;
    EXPECT_EQ(DecideApproach(o), V::Swim);
}

TEST(DungeonClearApproachTest, UnreachableWithWaterSwims)
{
    Observation o = Healthy();
    o.pathReachable = false;
    o.waterBetween = true;
    EXPECT_EQ(DecideApproach(o), V::Swim);
}

TEST(DungeonClearApproachTest, UnreachableLandlockedStalls)
{
    Observation o = Healthy();
    o.pathReachable = false;
    o.waterBetween = false;
    EXPECT_EQ(DecideApproach(o), V::Stall);
}

// ---- off-path resnap -----------------------------------------------------

TEST(DungeonClearApproachTest, OffPathRebuildsWhenReachable)
{
    Observation o = Healthy();
    o.offPath = true;
    EXPECT_EQ(DecideApproach(o), V::OffPathRebuild);
}

// ---- hop done: engageDist vs engageRange + escalation budget ------------

TEST(DungeonClearApproachTest, HopDoneInsideEngageRangeRebuildsAndYields)
{
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange - 1.0f;  // inside
    EXPECT_EQ(DecideApproach(o), V::RebuildAndYield);
}

TEST(DungeonClearApproachTest, HopDoneShortWithinBudgetFinalApproaches)
{
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange + 5.0f;  // outside engage range
    o.doneNotEngagedTicks = DONE_NOT_ENGAGED_LIMIT - 1;  // 14 < 15
    EXPECT_EQ(DecideApproach(o), V::FinalApproach);
}

TEST(DungeonClearApproachTest, HopDoneShortAtBudgetWithWaterSwims)
{
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange + 5.0f;
    o.doneNotEngagedTicks = DONE_NOT_ENGAGED_LIMIT;  // 15, budget exhausted
    o.waterBetween = true;
    EXPECT_EQ(DecideApproach(o), V::Swim);
}

TEST(DungeonClearApproachTest, HopDoneShortAtBudgetLandlockedStalls)
{
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange + 5.0f;
    o.doneNotEngagedTicks = DONE_NOT_ENGAGED_LIMIT;
    o.waterBetween = false;
    EXPECT_EQ(DecideApproach(o), V::Stall);
}

TEST(DungeonClearApproachTest, HopDoneEngageRangeBoundaryIsStrict)
{
    // engageDist == engageRange is NOT "inside" (the action uses < not <=), so
    // exactly at the boundary it escalates rather than yielding.
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange;  // not < engageRange
    o.doneNotEngagedTicks = 0;
    EXPECT_EQ(DecideApproach(o), V::FinalApproach);
}

// ---- jump / ride / off-line ordering ------------------------------------

TEST(DungeonClearApproachTest, JumpLegWhenHopIsJump)
{
    Observation o = Healthy();
    o.hopIsJump = true;
    EXPECT_EQ(DecideApproach(o), V::JumpLeg);
}

TEST(DungeonClearApproachTest, JumpOutranksRideAndOffLine)
{
    Observation o = Healthy();
    o.hopIsJump = true;
    o.splineRunning = true;
    o.offLine = true;
    EXPECT_EQ(DecideApproach(o), V::JumpLeg);
}

TEST(DungeonClearApproachTest, RideLiveGlideWhenSplineRunning)
{
    Observation o = Healthy();
    o.splineRunning = true;
    EXPECT_EQ(DecideApproach(o), V::RideLiveGlide);
}

TEST(DungeonClearApproachTest, RideOutranksOffLineAndWindow)
{
    Observation o = Healthy();
    o.splineRunning = true;
    o.offLine = true;
    EXPECT_EQ(DecideApproach(o), V::RideLiveGlide);
}

TEST(DungeonClearApproachTest, OffLineRejoinWhenDrifted)
{
    Observation o = Healthy();
    o.offLine = true;
    EXPECT_EQ(DecideApproach(o), V::OffLineRejoin);
}

TEST(DungeonClearApproachTest, OffLineOutranksSplineWindow)
{
    Observation o = Healthy();
    o.offLine = true;
    o.haveSplineWindow = true;
    EXPECT_EQ(DecideApproach(o), V::OffLineRejoin);
}

// ---- hop-done outranks the movement rungs -------------------------------

TEST(DungeonClearApproachTest, HopDoneOutranksJumpAndGlide)
{
    Observation o = Healthy();
    o.hopDone = true;
    o.engageDist = o.engageRange - 1.0f;  // -> RebuildAndYield
    o.hopIsJump = true;
    o.splineRunning = true;
    EXPECT_EQ(DecideApproach(o), V::RebuildAndYield);
}
