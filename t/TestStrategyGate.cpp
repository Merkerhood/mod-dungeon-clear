/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "DcStrategyGate.h"

// The dungeon-gate decision kernel is the pure core of the invariant
// "DC strategies installed iff the bot is on a dungeon/raid map". It is
// deliberately free of game types so the truth table can be pinned headlessly;
// the runtime Reconcile()/ReconcileAllBots() wrappers (game-coupled) are
// exercised live, per the plan's validation checklist.
using DcStrategyGate::Action;
using DcStrategyGate::Decide;

TEST(DungeonClearStrategyGate, InstallsWhenInDungeonAndMissing)
{
    EXPECT_EQ(Decide(/*inDungeon*/ true, /*hasStrategy*/ false), Action::Install);
}

TEST(DungeonClearStrategyGate, StripsWhenOutsideDungeonButPresent)
{
    EXPECT_EQ(Decide(/*inDungeon*/ false, /*hasStrategy*/ true), Action::Strip);
}

TEST(DungeonClearStrategyGate, NoOpWhenAlreadyCompliantInDungeon)
{
    EXPECT_EQ(Decide(/*inDungeon*/ true, /*hasStrategy*/ true), Action::None);
}

TEST(DungeonClearStrategyGate, NoOpWhenAlreadyCompliantOutsideDungeon)
{
    EXPECT_EQ(Decide(/*inDungeon*/ false, /*hasStrategy*/ false), Action::None);
}

// The kernel is constexpr — fold the whole truth table at compile time so a
// regression can't even build.
static_assert(Decide(true, false) == Action::Install, "in dungeon, missing -> install");
static_assert(Decide(false, true) == Action::Strip, "out of dungeon, present -> strip");
static_assert(Decide(true, true) == Action::None, "in dungeon, present -> none");
static_assert(Decide(false, false) == Action::None, "out of dungeon, missing -> none");
