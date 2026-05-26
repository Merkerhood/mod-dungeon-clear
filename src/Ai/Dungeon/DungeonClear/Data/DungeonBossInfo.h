/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONBOSSINFO_H
#define _PLAYERBOT_DUNGEONBOSSINFO_H

#include <string>

#include "Common.h"

struct DungeonBossInfo
{
    uint32 entry{0};
    uint32 encounterIndex{0};
    std::string name;
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

#endif
