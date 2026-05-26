/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BOSSOVERRIDEREGISTRY_H
#define _PLAYERBOT_BOSSOVERRIDEREGISTRY_H

#include <unordered_map>
#include <vector>

#include "Common.h"
#include "DBCEnums.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"

class BossOverrideRegistry
{
public:
    // Register a per-(mapId, difficulty) boss list. Intended to be called from
    // namespace-scope static initializers in per-dungeon override .cpp files.
    static void Register(uint32 mapId, Difficulty difficulty, std::vector<DungeonBossInfo> bosses);

    // Returns nullptr if no override is registered for the (mapId, difficulty).
    static std::vector<DungeonBossInfo> const* Get(uint32 mapId, Difficulty difficulty);

private:
    static std::unordered_map<uint64, std::vector<DungeonBossInfo>>& Store();

    static uint64 MakeKey(uint32 mapId, Difficulty difficulty)
    {
        return (static_cast<uint64>(mapId) << 32) | static_cast<uint32>(difficulty);
    }
};

#endif
