/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DcDifficultyGate.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcBossStandDown.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"

// Raid-core kernels (see deployment-files/docs/mod-dungeon-clear_raid-support_plan.md,
// Plan A): the DcDiffKey difficulty decode (incl. the 25-man-normal-is-not-heroic
// regression the raw-compare bugs would have shipped), the raid-aware difficulty
// gate, the boss stand-down hysteresis, and the roster skip-by-design /
// summoned-boss patch handling.

// --- DcDiffKey decode matrix ----------------------------------------------

TEST(DcDifficultyTest, DungeonTierDecode)
{
    EXPECT_FALSE(DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL).IsHeroic());
    EXPECT_TRUE(DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_HEROIC).IsHeroic());
    // Dungeon "epic" (2) counts as heroic-tier, mirroring Map::IsHeroic's >=.
    EXPECT_TRUE(DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_EPIC).IsHeroic());

    EXPECT_FALSE(DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL).Is25Man());
    EXPECT_FALSE(DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_HEROIC).Is25Man());
}

TEST(DcDifficultyTest, RaidTierDecode)
{
    // The regression the type exists to prevent: 25-man NORMAL shares raw value
    // 1 with dungeon heroic and must NOT read as heroic.
    EXPECT_FALSE(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL).IsHeroic());
    EXPECT_FALSE(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_NORMAL).IsHeroic());
    EXPECT_TRUE(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_HEROIC).IsHeroic());
    EXPECT_TRUE(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_HEROIC).IsHeroic());

    EXPECT_FALSE(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_NORMAL).Is25Man());
    EXPECT_TRUE(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL).Is25Man());
    EXPECT_FALSE(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_HEROIC).Is25Man());
    EXPECT_TRUE(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_HEROIC).Is25Man());
}

TEST(DcDifficultyTest, DefaultKeyIsDungeonNormal)
{
    DcDiffKey const key{};
    EXPECT_FALSE(key.isRaid);
    EXPECT_FALSE(key.IsHeroic());
    EXPECT_FALSE(key.Is25Man());
    EXPECT_TRUE(key == DcDiffKey::Dungeon(DUNGEON_DIFFICULTY_NORMAL));
}

// --- Gate matching across the raid axis -----------------------------------

TEST(DcDifficultyTest, GateDecodesRaidDifficultiesByTier)
{
    // Classic raids run at raid difficulty 0 and must read as normal-tier.
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::Any, DcDiffKey::Raid(0)));
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::NormalOnly, DcDiffKey::Raid(0)));
    EXPECT_FALSE(DcGateMatches(DcDifficultyGate::HeroicOnly, DcDiffKey::Raid(0)));

    // 25-man NORMAL (raw 1): the collision case. NormalOnly matches, HeroicOnly
    // must not — under the old raw-Difficulty compare both answers were wrong.
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::NormalOnly,
                              DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL)));
    EXPECT_FALSE(DcGateMatches(DcDifficultyGate::HeroicOnly,
                               DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL)));

    EXPECT_FALSE(DcGateMatches(DcDifficultyGate::NormalOnly,
                               DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_HEROIC)));
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::HeroicOnly,
                              DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_HEROIC)));
}

// --- DcBossStandDown::Update hysteresis -----------------------------------

TEST(DcBossStandDownTest, EntersInstantly)
{
    auto const v = DcBossStandDown::Update(/*wasActive*/ false, /*lastSignalMs*/ 0,
                                           /*signal*/ true, /*nowMs*/ 1000);
    EXPECT_TRUE(v.active);
    EXPECT_EQ(v.lastSignalMs, 1000u);
}

TEST(DcBossStandDownTest, QuietStaysInactive)
{
    auto const v = DcBossStandDown::Update(false, 0, false, 5000);
    EXPECT_FALSE(v.active);
    EXPECT_EQ(v.lastSignalMs, 0u);
}

TEST(DcBossStandDownTest, HoldsThroughSignalGapShorterThanGrace)
{
    // A submerge phase / RP pause blinks the signal off; inside the grace the
    // stand-down must hold so DC cannot flap back on mid-fight.
    auto v = DcBossStandDown::Update(false, 0, true, 10000);
    ASSERT_TRUE(v.active);
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, false,
                                10000 + DcBossStandDown::kExitGraceMs - 1);
    EXPECT_TRUE(v.active);
    // Signal returns before the grace elapses: re-arms the clock.
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, true, 14000);
    EXPECT_TRUE(v.active);
    EXPECT_EQ(v.lastSignalMs, 14000u);
}

TEST(DcBossStandDownTest, ExitsAfterGraceOfQuiet)
{
    auto v = DcBossStandDown::Update(false, 0, true, 10000);
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, false,
                                10000 + DcBossStandDown::kExitGraceMs);
    EXPECT_FALSE(v.active);
}

TEST(DcBossStandDownTest, ReentersAfterExit)
{
    auto v = DcBossStandDown::Update(false, 0, true, 1000);
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, false,
                                1000 + DcBossStandDown::kExitGraceMs);
    ASSERT_FALSE(v.active);
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, true, 20000);
    EXPECT_TRUE(v.active);
}

TEST(DcBossStandDownTest, MsWraparoundStillExits)
{
    // getMSTime wraps ~49 days in: unsigned subtraction must keep working.
    uint32 const nearWrap = 0xFFFFFF00u;
    auto v = DcBossStandDown::Update(false, 0, true, nearWrap);
    ASSERT_TRUE(v.active);
    uint32 const afterWrap = nearWrap + DcBossStandDown::kExitGraceMs;  // wraps
    v = DcBossStandDown::Update(v.active, v.lastSignalMs, false, afterWrap);
    EXPECT_FALSE(v.active);
}

// --- Roster patch: skip-by-design + summoned bosses ------------------------

namespace
{
    DungeonBossInfo MakeBoss(uint32 entry, uint32 encounterIndex)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = encounterIndex;
        b.name = "boss" + std::to_string(entry);
        return b;
    }
}

TEST(DcRaidRosterTest, SkipByDesignStampsKeptEntries)
{
    BossRosterPatch patch;
    patch.mapId = 999;
    patch.skipByDesign = {20};

    std::vector<DungeonBossInfo> base{MakeBoss(10, 0), MakeBoss(20, 1), MakeBoss(30, 2)};
    auto const out = BossRosterRegistry::ApplyPatch(patch, base);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_FALSE(out[0].skipByDesign);
    EXPECT_TRUE(out[1].skipByDesign);
    EXPECT_EQ(out[1].entry, 20u);
    EXPECT_FALSE(out[2].skipByDesign);
}

TEST(DcRaidRosterTest, SummonedBossAddKeepsOrderAndCarriesNoSpawnAssumptions)
{
    // The summoned-boss seam: a boss with NO creature spawn rows (MC's
    // Majordomo/Ragnaros, BWL's Nefarian) enters the roster via a patch `add`
    // with authored coords and its real DBC encounterIndex. It must slot into
    // clear order like any other anchor; liveness/present handling is the
    // picker's job (absent from the spawn store reads as not-yet-streamed).
    BossRosterPatch patch;
    patch.mapId = 409;
    DungeonBossInfo summoned = MakeBoss(11502, 9);  // Ragnaros
    summoned.x = 838.0f; summoned.y = -829.0f; summoned.z = -232.0f;
    patch.add = {summoned};

    std::vector<DungeonBossInfo> base{MakeBoss(12118, 0), MakeBoss(11982, 1)};
    auto const out = BossRosterRegistry::ApplyPatch(patch, base);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out.back().entry, 11502u);      // highest encounterIndex sorts last
    EXPECT_EQ(out.back().encounterIndex, 9u); // completion keys on the DBC bit
    EXPECT_FALSE(out.back().skipByDesign);
}
