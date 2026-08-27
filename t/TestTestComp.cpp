/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "TestRun/DcTestComp.h"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>

using DcTestComp::BuildComp;
using DcTestComp::kPartySize;
using DcTestComp::RolePool;
using DcTestComp::Slot;

namespace
{
    // Death knight (6) is excluded; every other pooled class is one of these.
    bool IsKnownClass(std::uint8_t c)
    {
        return c == 1 || c == 2 || c == 3 || c == 4 || c == 5 ||
               c == 7 || c == 8 || c == 9 || c == 11;
    }
}

// Same seed -> byte-identical comp (classes and specs). This is what lets a
// bug-tripping run be replayed via `.dc test start <d> seed=N`.
TEST(DcTestComp, DeterministicForSameSeed)
{
    for (std::uint32_t seed : {1u, 7u, 42u, 1000u, 0xABCDEF12u})
    {
        auto a = BuildComp(seed);
        auto b = BuildComp(seed);
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_EQ(a[i].classId, b[i].classId) << "seed=" << seed << " slot=" << i;
            EXPECT_STREQ(a[i].specName, b[i].specName) << "seed=" << seed << " slot=" << i;
        }
    }
}

// Slot 0 is always the tank (leader), slot 1 the healer, slots 2-4 DPS.
TEST(DcTestComp, RoleLayoutIsStable)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed);
        EXPECT_STREQ(c[0].role, "tank") << "seed=" << seed;
        EXPECT_STREQ(c[1].role, "heal") << "seed=" << seed;
        for (std::size_t i = 2; i < kPartySize; ++i)
            EXPECT_STREQ(c[i].role, "dps") << "seed=" << seed << " slot=" << i;
    }
}

// All five bots are on distinct, known (non-DK) classes with a named spec.
TEST(DcTestComp, DistinctKnownClassesWithSpecs)
{
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed);
        std::set<std::uint8_t> classes;
        for (std::size_t i = 0; i < kPartySize; ++i)
        {
            EXPECT_TRUE(IsKnownClass(c[i].classId))
                << "seed=" << seed << " classId=" << int(c[i].classId);
            EXPECT_NE(c[i].classId, 6) << "death knight leaked into comp, seed=" << seed;
            ASSERT_NE(c[i].specName, nullptr);
            EXPECT_NE(std::string_view(c[i].specName), std::string_view())
                << "empty spec, seed=" << seed << " slot=" << i;
            classes.insert(c[i].classId);
        }
        EXPECT_EQ(classes.size(), kPartySize) << "duplicate class, seed=" << seed;
    }
}

// The point of the feature: comps actually vary. Over many seeds we expect
// more than one tank class, more than one healer class, and a healthy spread
// of DPS classes — otherwise the "randomisation" is stuck on one shape.
TEST(DcTestComp, ActuallyVaries)
{
    std::set<std::uint8_t> tanks, heals, dps;
    for (std::uint32_t seed = 0; seed < 500; ++seed)
    {
        auto c = BuildComp(seed);
        tanks.insert(c[0].classId);
        heals.insert(c[1].classId);
        for (std::size_t i = 2; i < kPartySize; ++i)
            dps.insert(c[i].classId);
    }
    EXPECT_GT(tanks.size(), 1u);
    EXPECT_GT(heals.size(), 1u);
    EXPECT_GE(dps.size(), 5u);
}

TEST(DcTestComp, RolePoolLookup)
{
    EXPECT_FALSE(RolePool("tank").empty());
    EXPECT_FALSE(RolePool("heal").empty());
    EXPECT_FALSE(RolePool("dps").empty());
    EXPECT_TRUE(RolePool("bogus").empty());

    // Every pool entry carries the role token it was fetched under.
    for (Slot const& s : RolePool("tank"))
        EXPECT_STREQ(s.role, "tank");
    for (Slot const& s : RolePool("heal"))
        EXPECT_STREQ(s.role, "heal");
    for (Slot const& s : RolePool("dps"))
        EXPECT_STREQ(s.role, "dps");
}

// --- Sized (raid) comps — raid-support Plan D ------------------------------

TEST(DcTestComp, RoleQuotaTable)
{
    using DcTestComp::RoleQuota;
    auto q = RoleQuota(5);
    EXPECT_EQ(q.tanks, 1u);
    EXPECT_EQ(q.healers, 1u);
    EXPECT_EQ(q.dps, 3u);

    q = RoleQuota(10);
    EXPECT_EQ(q.tanks, 2u);
    EXPECT_EQ(q.healers, 2u);
    EXPECT_EQ(q.dps, 6u);

    q = RoleQuota(25);
    EXPECT_EQ(q.tanks, 3u);
    EXPECT_EQ(q.healers, 6u);
    EXPECT_EQ(q.dps, 16u);

    q = RoleQuota(40);
    EXPECT_EQ(q.tanks, 4u);
    EXPECT_EQ(q.healers, 10u);
    EXPECT_EQ(q.dps, 26u);

    // Tiny sizes never quota away the whole party.
    q = RoleQuota(2);
    EXPECT_EQ(q.tanks + q.healers + q.dps, 2u);
    EXPECT_GE(q.tanks, 1u);
}

TEST(DcTestComp, SizedCompIsDeterministicAndQuotaed)
{
    auto const a = DcTestComp::BuildComp(1234u, 25);
    auto const b = DcTestComp::BuildComp(1234u, 25);
    ASSERT_EQ(a.size(), 25u);
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(a[i].classId, b[i].classId) << i;
        EXPECT_STREQ(a[i].specName, b[i].specName) << i;
    }
    std::size_t tanks = 0, heals = 0, dps = 0;
    for (auto const& s : a)
    {
        if (std::string_view(s.role) == "tank") ++tanks;
        else if (std::string_view(s.role) == "heal") ++heals;
        else ++dps;
    }
    EXPECT_EQ(tanks, 3u);
    EXPECT_EQ(heals, 6u);
    EXPECT_EQ(dps, 16u);
}

TEST(DcTestComp, SizedCompSpreadsDuplicatesEvenly)
{
    // 6 healers over a 4-class heal pool (priest twice via disc/holy): the
    // least-used draw means no class is used 3+ times while another is unused.
    auto const comp = DcTestComp::BuildComp(777u, 25);
    std::map<std::uint8_t, std::size_t> healUse;
    for (auto const& s : comp)
        if (std::string_view(s.role) == "heal")
            ++healUse[s.classId];
    std::size_t maxUse = 0, minUse = SIZE_MAX;
    for (auto const& kv : healUse)
    {
        maxUse = std::max(maxUse, kv.second);
        minUse = std::min(minUse, kv.second);
    }
    EXPECT_LE(maxUse - minUse, 1u);  // even spread across the classes drawn
}

TEST(DcTestComp, SizeIsClamped)
{
    EXPECT_EQ(DcTestComp::BuildComp(1u, 1).size(), DcTestComp::kMinPartySize);
    EXPECT_EQ(DcTestComp::BuildComp(1u, 99).size(), DcTestComp::kMaxPartySize);
}
