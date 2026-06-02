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

            // --- Scarlet Monastery (map 189) -----------------------------
            // Four wings, each entered through its own portal off the shared
            // outdoor courtyard; you must leave to the courtyard to switch, so
            // no in-instance route connects them. The wing clusters sit far
            // apart in world space — Graveyard (x~1800, y~1270) and Cathedral
            // (x~1160, y~1370) in the north half, Library (x~130, y~-345) and
            // Armory (x~1965, y~-430) in the south half, each 600+ yds from the
            // others — so nearest-boss wing detection is unambiguous.
            //
            // Entries are the kill-creature credit-entries from
            // instance_encounters (what BossSpawnIndex emits), NOT every
            // lore boss: the Cathedral's tracked encounters are Fairbanks and
            // Whitemane — Scarlet Commander Mograine (3976) has no
            // DungeonEncounter row, so he never appears in the boss list and
            // must not be listed here.
            {189, {
                {"Scarlet Monastery (Graveyard)", {
                    3983,   // Interrogator Vishas
                    4543,   // Bloodmage Thalnos
                }},
                {"Scarlet Monastery (Library)", {
                    3974,   // Houndmaster Loksey
                    6487,   // Arcanist Doan
                }},
                {"Scarlet Monastery (Armory)", {
                    3975,   // Herod
                }},
                {"Scarlet Monastery (Cathedral)", {
                    4542,   // High Inquisitor Fairbanks
                    3977,   // High Inquisitor Whitemane
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
