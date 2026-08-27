/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestComp.h"

#include <map>
#include <set>

namespace DcTestComp
{
    namespace
    {
        // splitmix32: a tiny, self-contained, deterministic PRNG. We can't lean
        // on the project's global urand()/rand32() here because BuildComp must
        // be PURE (same seed -> same comp, for replay and unit tests); those
        // draw from shared world state. This is comp selection, not gameplay
        // randomness, so distribution quality is irrelevant.
        std::uint32_t NextRand(std::uint32_t& state)
        {
            state += 0x9e3779b9u;
            std::uint32_t z = state;
            z = (z ^ (z >> 16)) * 0x21f0aaadu;
            z = (z ^ (z >> 15)) * 0x735a2d97u;
            return z ^ (z >> 15);
        }

        // Draw one entry from `pool` whose class is not already used, advancing
        // the PRNG. Callers guarantee at least one such entry exists.
        Slot Draw(Slot const* pool, std::size_t count, std::set<std::uint8_t> const& used,
                  std::uint32_t& state)
        {
            std::vector<Slot> candidates;
            candidates.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                if (used.find(pool[i].classId) == used.end())
                    candidates.push_back(pool[i]);

            // Defensive: should never happen given the pool sizes, but returning
            // a valid slot beats indexing an empty vector.
            if (candidates.empty())
                return pool[0];

            return candidates[NextRand(state) % candidates.size()];
        }

        // Raid-scale draw: duplicates allowed, but always from the LEAST-used
        // classes of the pool, so 25 slots over 9 classes spread as evenly as
        // the role pools permit instead of stacking one lucky class.
        Slot DrawSpread(Slot const* pool, std::size_t count,
                        std::map<std::uint8_t, std::size_t>& classUse,
                        std::uint32_t& state)
        {
            std::size_t best = SIZE_MAX;
            for (std::size_t i = 0; i < count; ++i)
            {
                auto const it = classUse.find(pool[i].classId);
                std::size_t const uses = it == classUse.end() ? 0 : it->second;
                if (uses < best)
                    best = uses;
            }
            std::vector<Slot> candidates;
            candidates.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                auto const it = classUse.find(pool[i].classId);
                std::size_t const uses = it == classUse.end() ? 0 : it->second;
                if (uses == best)
                    candidates.push_back(pool[i]);
            }
            Slot const pick = candidates[NextRand(state) % candidates.size()];
            ++classUse[pick.classId];
            return pick;
        }
    }

    Quota RoleQuota(std::size_t size)
    {
        if (size < kMinPartySize)
            size = kMinPartySize;
        if (size > kMaxPartySize)
            size = kMaxPartySize;

        Quota q{};
        q.tanks = size <= 9 ? 1 : size <= 15 ? 2 : size <= 30 ? 3 : 4;
        q.healers = size / 4;
        if (q.healers == 0)
            q.healers = 1;
        // Never quota away the last DPS on tiny sizes (size 2 = tank + healer).
        while (q.tanks + q.healers > size)
            q.healers > 1 ? --q.healers : --q.tanks;
        q.dps = size - q.tanks - q.healers;
        return q;
    }

    std::array<Slot, kPartySize> BuildComp(std::uint32_t seed)
    {
        std::uint32_t state = seed;
        std::set<std::uint8_t> used;
        std::array<Slot, kPartySize> comp{};

        comp[0] = Draw(kTankPool, std::size(kTankPool), used, state);
        used.insert(comp[0].classId);

        comp[1] = Draw(kHealPool, std::size(kHealPool), used, state);
        used.insert(comp[1].classId);

        for (std::size_t i = 2; i < kPartySize; ++i)
        {
            comp[i] = Draw(kDpsPool, std::size(kDpsPool), used, state);
            used.insert(comp[i].classId);
        }

        return comp;
    }

    std::vector<Slot> BuildComp(std::uint32_t seed, std::size_t size)
    {
        Quota const q = RoleQuota(size);
        std::uint32_t state = seed;
        std::map<std::uint8_t, std::size_t> classUse;
        std::vector<Slot> comp;
        comp.reserve(q.tanks + q.healers + q.dps);

        for (std::size_t i = 0; i < q.tanks; ++i)
            comp.push_back(DrawSpread(kTankPool, std::size(kTankPool), classUse, state));
        for (std::size_t i = 0; i < q.healers; ++i)
            comp.push_back(DrawSpread(kHealPool, std::size(kHealPool), classUse, state));
        for (std::size_t i = 0; i < q.dps; ++i)
            comp.push_back(DrawSpread(kDpsPool, std::size(kDpsPool), classUse, state));
        return comp;
    }

    std::vector<Slot> RolePool(std::string_view role)
    {
        if (role == "tank")
            return { std::begin(kTankPool), std::end(kTankPool) };
        if (role == "heal")
            return { std::begin(kHealPool), std::end(kHealPool) };
        if (role == "dps")
            return { std::begin(kDpsPool), std::end(kDpsPool) };
        return {};
    }
}
