/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTargetExclusionRegistry.h"

#include "GameObject.h"
#include "InstanceScript.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include <list>

namespace
{
    // Blackwing Lair: Razorgore, for as long as an egg is standing.
    //
    // Phase is read from the instance's own DATA_EGG_EVENT, which flips to DONE
    // the instant the thirtieth egg breaks. The obvious alternative — "are there
    // eggs near me" — is wrong in the one direction that matters: a scan is
    // bounded by range, so a raid that has drifted away from the last few eggs
    // reads phase 2, opens up on the boss, and wipes on 20038. The scan survives
    // only as the fallback for a map with no instance script, where it is better
    // than nothing.
    bool RazorgoreEggPhase(Player* bot)
    {
        if (!bot)
            return false;

        if (InstanceScript* inst = bot->GetInstanceScript())
            return inst->GetData(DcBlackwingLair::DATA_EGG_EVENT) != DONE;

        std::list<GameObject*> eggs;
        bot->GetGameObjectListWithEntryInGrid(eggs, DcBlackwingLair::GO_BLACK_DRAGON_EGG,
                                              DcBlackwingLair::ROOM_SCAN);
        for (GameObject* egg : eggs)
            if (egg && egg->isSpawned() && egg->GetGoState() == GO_STATE_READY)
                return true;
        return false;
    }

    DcTargetExclusionRow const kRows[] = {
        // Blackwing Lair — Razorgore the Untamed. Killing him before the last egg
        // breaks casts 20038 (Explosion) and instakills the raid, so phase-1
        // damage into him does not merely go to waste: it ends the run, and with
        // 10-40 bots it takes about as long as the egg run does. Tanks are NOT
        // excluded — an off-tank holding him between mind controls is how the
        // fight is played, and the exclusion types below leave that alone.
        { 469, 12435, &RazorgoreEggPhase },
    };
}

bool DcTargetExclusionRegistry::HasRowsFor(uint32 mapId)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId)
            return true;
    return false;
}

bool DcTargetExclusionRegistry::IsExcluded(Player* bot, uint32 mapId, uint32 entry)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId && r.entry == entry)
            return !r.inForce || r.inForce(bot);
    return false;
}
