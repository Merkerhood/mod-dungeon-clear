/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcRaidMusterDecision.h"

// The raid pre-boss muster phase machine (raid-support plan, Plan C):
// Idle -> Resting -> Rebuffing -> Ready, every phase timeout-bounded.

using DcRaidMusterDecision::Decide;
using DcRaidMusterDecision::Inputs;
using DcRaidMusterDecision::Phase;

namespace
{
    Inputs In(bool staged, bool topped, bool rebuffDone, std::uint32_t now,
              std::uint32_t since, std::uint32_t restMs = 120000, std::uint32_t rebuffMs = 45000)
    {
        Inputs in;
        in.staged = staged;
        in.topped = topped;
        in.rebuffDone = rebuffDone;
        in.nowMs = now;
        in.phaseSinceMs = since;
        in.restTimeoutMs = restMs;
        in.rebuffTimeoutMs = rebuffMs;
        return in;
    }
}

TEST(DcRaidMusterTest, IdleArmsIntoRestingAndHolds)
{
    auto const v = Decide(Phase::Idle, In(false, false, false, 1000, 0));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    EXPECT_FALSE(v.beginRebuff);
}

TEST(DcRaidMusterTest, RestingHoldsUntilStagedAndTopped)
{
    // Staged but not topped: hold.
    auto v = Decide(Phase::Resting, In(true, false, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    // Topped but a straggler outside: hold — the boss gate is strict.
    v = Decide(Phase::Resting, In(false, true, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
    // Both: advance to the rebuff round, still holding this tick.
    v = Decide(Phase::Resting, In(true, true, false, 5000, 1000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.hold);
    EXPECT_TRUE(v.beginRebuff);
    EXPECT_FALSE(v.timedOut);
}

TEST(DcRaidMusterTest, RestingTimesOutIntoRebuff)
{
    auto const v = Decide(Phase::Resting, In(false, false, false,
                                             /*now*/ 121001, /*since*/ 1000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.beginRebuff);
    EXPECT_TRUE(v.timedOut);
    EXPECT_TRUE(v.hold);
}

TEST(DcRaidMusterTest, RebuffHoldsUntilDoneThenReleases)
{
    auto v = Decide(Phase::Rebuffing, In(true, true, false, 10000, 6000));
    EXPECT_EQ(v.phase, Phase::Rebuffing);
    EXPECT_TRUE(v.hold);
    v = Decide(Phase::Rebuffing, In(true, true, true, 12000, 6000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_FALSE(v.timedOut);
}

TEST(DcRaidMusterTest, RebuffTimesOutIntoReady)
{
    auto const v = Decide(Phase::Rebuffing, In(true, true, false,
                                               /*now*/ 51001, /*since*/ 6000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
    EXPECT_TRUE(v.timedOut);
}

TEST(DcRaidMusterTest, ReadyStaysReleased)
{
    auto const v = Decide(Phase::Ready, In(false, false, false, 99000, 50000));
    EXPECT_EQ(v.phase, Phase::Ready);
    EXPECT_FALSE(v.hold);
}

TEST(DcRaidMusterTest, ZeroTimeoutNeverExpires)
{
    auto const v = Decide(Phase::Resting,
                          In(false, false, false, 999999, 1, /*rest*/ 0));
    EXPECT_EQ(v.phase, Phase::Resting);
    EXPECT_TRUE(v.hold);
}
