/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossOverrideRegistry.h"

std::unordered_map<uint64, std::vector<DungeonBossInfo>>& BossOverrideRegistry::Store()
{
    static std::unordered_map<uint64, std::vector<DungeonBossInfo>> instance;
    return instance;
}

void BossOverrideRegistry::Register(uint32 mapId, Difficulty difficulty, std::vector<DungeonBossInfo> bosses)
{
    Store()[MakeKey(mapId, difficulty)] = std::move(bosses);
}

std::vector<DungeonBossInfo> const* BossOverrideRegistry::Get(uint32 mapId, Difficulty difficulty)
{
    auto const& s = Store();
    auto it = s.find(MakeKey(mapId, difficulty));
    if (it == s.end())
        return nullptr;
    return &it->second;
}
