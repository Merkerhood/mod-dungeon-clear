/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonBossesValue.h"

#include "Map.h"
#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossOverrideRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Playerbots.h"

namespace
{
    // Snap each boss's spawn position onto the navmesh. DBC / creature_data
    // coordinates sometimes sit a few yards above the floor (alcove ledges,
    // platforms where the spawn was placed before mmap regen, etc.). Without
    // snapping, PathGenerator returns PATHFIND_FARFROMPOLY_END for those
    // bosses and Advance's at-boss trigger never matches the raw spawn pos.
    // 40yd is generous enough to catch flying-phase bosses whose anchor is
    // suspended in air (Eregos, Skadi's drake-phase landing) without
    // wandering into a different room.
    constexpr float BOSS_SNAP_RADIUS = 40.0f;

    std::vector<DungeonBossInfo> SnapAll(Map const* map, std::vector<DungeonBossInfo> bosses)
    {
        if (!map)
            return bosses;
        for (DungeonBossInfo& info : bosses)
        {
            NavmeshSnap::Result const r = NavmeshSnap::Snap(map, info.x, info.y, info.z, BOSS_SNAP_RADIUS);
            if (r.ok)
            {
                info.x = r.x;
                info.y = r.y;
                info.z = r.z;
            }
            // Snap failure leaves the original coords. The at-boss trigger
            // will then never fire for that boss and the stalled fallback
            // takes over — same behavior as before this change.
        }
        return bosses;
    }
}

std::vector<DungeonBossInfo> DungeonBossesValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return {};

    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return {};

    uint32 const mapId = bot->GetMapId();
    Difficulty const difficulty = map->GetDifficulty();

    if (std::vector<DungeonBossInfo> const* over = BossOverrideRegistry::Get(mapId, difficulty))
        return SnapAll(map, *over);

    return SnapAll(map, BossSpawnIndex::Get(mapId, difficulty));
}
