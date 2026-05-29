/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONWINGREGISTRY_H
#define _PLAYERBOT_DUNGEONWINGREGISTRY_H

#include <string>
#include <vector>

#include "Common.h"

// Some 3.3.5 dungeons pack several mutually-inaccessible "wings" onto a single
// instance map. Dire Maul (map 429) is the canonical case: East, West and North
// share one map id but you enter each through its own portal and cannot reach
// the others from inside. DungeonEncounter.dbc lists all wings' bosses under
// that one map id, so the stock (mapId, difficulty) boss list mixes every
// wing's bosses together — the bot then targets bosses it can never reach and
// the dungeon never reads as "cleared".
//
// This registry records, per split map, which boss credit-entries belong to
// which wing. DungeonBossesValue uses it to keep only the bosses of the wing
// the bot is actually standing in (picked by proximity — the wings are far
// apart in world space, so the nearest registered boss is always in-wing).
//
// Maps NOT listed here are single-wing and pass through unfiltered. The lists
// are static game data (vanilla dungeon layouts never change), so they live in
// code rather than the DB.
struct DungeonWing
{
    std::string name;                  // human-readable, for logs/chat
    std::vector<uint32> bossEntries;   // creature credit-entries in this wing
};

class DungeonWingRegistry
{
public:
    // Returns the wing breakdown for a split map, or nullptr if the map is not
    // split into wings (the common case — caller then keeps the full list).
    static std::vector<DungeonWing> const* Get(uint32 mapId);
};

#endif
