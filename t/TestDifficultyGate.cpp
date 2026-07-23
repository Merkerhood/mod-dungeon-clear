/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DcDifficultyGate.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

// Heroic-dungeon difficulty plumbing (see
// deployment-files/docs/mod-dungeon-clear_heroic-dungeons_plan.md): the gate
// type itself, the roster registry's difficulty-aware Apply, the route
// registry's normal-difficulty fallback, and the Conditional-event difficulty
// overload.

// --- DcGateMatches truth table --------------------------------------------

TEST(DcDifficultyGateTest, GateMatchesTruthTable)
{
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::Any, DUNGEON_DIFFICULTY_NORMAL));
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::Any, DUNGEON_DIFFICULTY_HEROIC));

    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::NormalOnly, DUNGEON_DIFFICULTY_NORMAL));
    EXPECT_FALSE(DcGateMatches(DcDifficultyGate::NormalOnly, DUNGEON_DIFFICULTY_HEROIC));

    EXPECT_FALSE(DcGateMatches(DcDifficultyGate::HeroicOnly, DUNGEON_DIFFICULTY_NORMAL));
    EXPECT_TRUE(DcGateMatches(DcDifficultyGate::HeroicOnly, DUNGEON_DIFFICULTY_HEROIC));
}

// --- BossRosterRegistry::Apply is difficulty-aware ------------------------

namespace
{
    DungeonBossInfo GateBoss(uint32 entry, uint32 idx, char const* name, uint32 mapId)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = idx;
        b.name = name;
        b.mapId = mapId;
        return b;
    }
}

TEST(DcDifficultyGateTest, AnyGatedPatchAppliesOnBothDifficulties)
{
    // Every currently registered patch is gate Any, so a patched map must
    // produce the SAME roster on normal and heroic (Hellfire Ramparts: the
    // Vazruden add lands on both). This pins the default-gate behaviour so
    // heroic runs don't silently lose the hand-authored corrections.
    std::vector<DungeonBossInfo> base = {
        GateBoss(17306, 0, "Watchkeeper Gargolmar", 543),
        GateBoss(17308, 1, "Omor the Unscarred", 543),
    };

    std::vector<DungeonBossInfo> normal =
        BossRosterRegistry::Apply(543, DUNGEON_DIFFICULTY_NORMAL, base);
    std::vector<DungeonBossInfo> heroic =
        BossRosterRegistry::Apply(543, DUNGEON_DIFFICULTY_HEROIC, base);

    ASSERT_EQ(normal.size(), heroic.size());
    for (size_t i = 0; i < normal.size(); ++i)
    {
        EXPECT_EQ(normal[i].entry, heroic[i].entry);
        EXPECT_EQ(normal[i].encounterIndex, heroic[i].encounterIndex);
    }
}

TEST(DcDifficultyGateTest, RegisteredPatchesDefaultToAnyGate)
{
    // Phase-1 invariant: no content has authored a difficulty-gated patch yet,
    // so everything must carry the Any default. When Phase 4 adds the first
    // HeroicOnly patch (Shattered Halls / Sethekk), replace this with a lint
    // that gated patches sit alongside an Any base patch for the same map.
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
        EXPECT_EQ(patch.gate, DcDifficultyGate::Any)
            << "map " << patch.mapId << " registered a gated patch — update this lint";
}

// --- Route registry falls back to normal ----------------------------------

TEST(DcDifficultyGateTest, RouteRegistryHeroicFallsBackToNormalRow)
{
    // Synthetic map far outside real content so the static store is not
    // polluted for other suites' lookups.
    constexpr uint32 kMap = 999901;
    constexpr uint32 kBoss = 42;

    WaypointHint normalHint;
    normalHint.x = 1.0f;
    DungeonClearRouteRegistry::Register(kMap, DUNGEON_DIFFICULTY_NORMAL, kBoss, {normalHint});

    // Heroic lookup with no heroic row: the normal route is returned.
    std::vector<WaypointHint> const* viaFallback =
        DungeonClearRouteRegistry::Get(kMap, DUNGEON_DIFFICULTY_HEROIC, kBoss);
    ASSERT_NE(viaFallback, nullptr);
    EXPECT_FLOAT_EQ((*viaFallback)[0].x, 1.0f);

    // A heroic-specific row, once registered, wins over the fallback.
    WaypointHint heroicHint;
    heroicHint.x = 2.0f;
    DungeonClearRouteRegistry::Register(kMap, DUNGEON_DIFFICULTY_HEROIC, kBoss, {heroicHint});
    std::vector<WaypointHint> const* heroicRow =
        DungeonClearRouteRegistry::Get(kMap, DUNGEON_DIFFICULTY_HEROIC, kBoss);
    ASSERT_NE(heroicRow, nullptr);
    EXPECT_FLOAT_EQ((*heroicRow)[0].x, 2.0f);

    // A boss with no row on either difficulty still misses cleanly.
    EXPECT_EQ(DungeonClearRouteRegistry::Get(kMap, DUNGEON_DIFFICULTY_HEROIC, kBoss + 1), nullptr);
}

// --- Conditional(mapId, difficulty) overload ------------------------------

TEST(DcDifficultyGateTest, ConditionalOverloadFiltersExactlyByGate)
{
    // For every map with conditional events, the difficulty overload must keep
    // exactly the events whose gate matches — no more (a gated event leaking
    // into the wrong difficulty) and no less (an Any event dropped). Holds for
    // today's all-Any table and stays correct once gated content lands.
    for (DungeonEvent const& seed : DungeonEventRegistry::AllEvents())
    {
        std::vector<DungeonEvent const*> const all =
            DungeonEventRegistry::Conditional(seed.mapId);
        if (all.empty())
            continue;

        size_t expectNormal = 0;
        size_t expectHeroic = 0;
        for (DungeonEvent const* ev : all)
        {
            if (DcGateMatches(ev->gate, DUNGEON_DIFFICULTY_NORMAL))
                ++expectNormal;
            if (DcGateMatches(ev->gate, DUNGEON_DIFFICULTY_HEROIC))
                ++expectHeroic;
        }

        EXPECT_EQ(DungeonEventRegistry::Conditional(seed.mapId, DUNGEON_DIFFICULTY_NORMAL).size(),
                  expectNormal) << "map " << seed.mapId;
        EXPECT_EQ(DungeonEventRegistry::Conditional(seed.mapId, DUNGEON_DIFFICULTY_HEROIC).size(),
                  expectHeroic) << "map " << seed.mapId;
    }
}
