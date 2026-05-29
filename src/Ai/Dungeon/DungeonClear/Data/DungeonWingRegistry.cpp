/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonWingRegistry.h"

#include <unordered_map>

namespace
{
    // mapId -> wings. Boss entries are the ENCOUNTER_CREDIT_KILL_CREATURE
    // credit-entries from instance_encounters, i.e. exactly what BossSpawnIndex
    // keys its list on, so a wing's entries match the "dungeon bosses" output
    // 1:1. Every boss of a split map must appear in exactly one wing — any
    // entry left out would silently never be cleared.
    std::unordered_map<uint32, std::vector<DungeonWing>> const& Store()
    {
        static std::unordered_map<uint32, std::vector<DungeonWing>> const store = {
            // --- Dire Maul (map 429) -------------------------------------
            // Three wings, each entered through its own portal; no in-instance
            // route connects them. Grouping verified against creature spawn
            // coords (East sits at y < -100, West at x < 150 / y > 400, North
            // at x > 300), which is also why nearest-boss wing detection is
            // unambiguous.
            {429, {
                {"Dire Maul (East)", {
                    11490,  // Zevrim Thornhoof
                    13280,  // Hydrospawn
                    14327,  // Lethtendris
                    11492,  // Alzzin the Wildshaper
                }},
                {"Dire Maul (West)", {
                    11489,  // Tendris Warpwood
                    11488,  // Illyanna Ravenoak
                    11487,  // Magister Kalendris
                    11496,  // Immol'thar
                    11486,  // Prince Tortheldrin
                }},
                {"Dire Maul (North)", {
                    14326,  // Guard Mol'dar
                    14322,  // Stomper Kreeg
                    14321,  // Guard Fengus
                    14323,  // Guard Slip'kik
                    14325,  // Captain Kromcrush
                    14324,  // Cho'Rush the Observer
                    11501,  // King Gordok
                }},
            }},
        };
        return store;
    }
}

std::vector<DungeonWing> const* DungeonWingRegistry::Get(uint32 mapId)
{
    auto const& s = Store();
    auto it = s.find(mapId);
    return it == s.end() ? nullptr : &it->second;
}
