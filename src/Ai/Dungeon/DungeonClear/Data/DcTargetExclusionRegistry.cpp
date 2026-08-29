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

    // Blackwing Lair: the drake hall, while Broodlord stands AND the bot is on the
    // wrong floor for it.
    //
    // TWO CLAUSES, and the second is what keeps the bar from outliving its reason.
    //
    // Broodlord's state comes from the instance's own boss slot rather than a
    // scan: Firemaw is the near end of a hall the raid does not reach for another
    // 340yd, so there is nothing local to look at, and BROODLORD_ENCOUNTER_INDEX
    // is authored from instance_blackwing_lair's enum rather than derived from the
    // roster (see the note on that constant). No instance script — never in a real
    // run of this map — reads as "still standing", the safe direction.
    //
    // THE FLOOR TEST IS THE ONE THAT MATTERS. What makes this pull wrong is not
    // distance — approach anchor 7 is 24.7yd from Firemaw, closer than most
    // legitimate pulls — it is that the 24.6yd of it are VERTICAL, with a floor in
    // between and no navmesh route across. A bot below the drake hall's floor
    // cannot be in a real fight with anything standing on it. A bot ON that floor
    // can, so the bar lifts there whatever the boss states say: that is what keeps
    // a run whose operator typed `dc skip` on Broodlord from arriving in the hall
    // to find nobody willing to shoot.
    constexpr float BWL_DRAKE_HALL_FLOOR_Z = 445.0f;  // hall 449.1; approach tops out at 438.6

    bool BwlDrakeHallOutOfOrder(Player* bot)
    {
        if (!bot)
            return false;

        if (bot->GetPositionZ() >= BWL_DRAKE_HALL_FLOOR_Z)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        return !inst || inst->GetBossState(DcBlackwingLair::BROODLORD_ENCOUNTER_INDEX) != DONE;
    }

    DcTargetExclusionRow const kRows[] = {
        // Blackwing Lair — Razorgore the Untamed. Killing him before the last egg
        // breaks casts 20038 (Explosion) and instakills the raid, so phase-1
        // damage into him does not merely go to waste: it ends the run, and with
        // 10-40 bots it takes about as long as the egg run does. Tanks are NOT
        // excluded — an off-tank holding him between mind controls is how the
        // fight is played, and the exclusion types below leave that alone.
        { 469, 12435, &RazorgoreEggPhase },

        // Blackwing Lair — the drake hall, while Broodlord is still alive.
        //
        // This is not a damage-efficiency row, it is a DO NOT GO THERE row. The
        // authored approach to the Suppression Rooms walks the whole raid up a
        // hall 24.7yd directly beneath Firemaw; one bot inside his aggro radius
        // puts every player on the map into combat with him through
        // `DoZoneInCombat`, which tests neither line of sight nor reachability.
        // Nothing in this module can stop that flag. What it CAN stop is the raid
        // answering it: with these rows nobody picks him, so nobody chases him
        // through a ceiling the navmesh has no route across, the raid does not
        // split over two floors for the rest of the run, and Firemaw is not killed
        // out of order and then skipped. Measured in tp-20260828-121941-1: four of
        // five runs, all twenty-five bots in combat with him inside one second at
        // 21.8-61.5yd while he sat on his spawn.
        //
        // `alsoTank` on all four: the tank answering a boss two rooms ahead is
        // precisely how the raid ends up there.
        //
        // The window closes when Broodlord dies OR when the raid is standing on
        // the hall's own floor — so this can never block the kills it is
        // sequencing, by either route into them.
        { 469, DcBlackwingLair::NPC_FIREMAW,    &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_EBONROC,    &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_FLAMEGOR,   &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
        { 469, DcBlackwingLair::NPC_CHROMAGGUS, &BwlDrakeHallOutOfOrder, /*alsoTank*/ true },
    };
}

bool DcTargetExclusionRegistry::HasRowsFor(uint32 mapId)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId)
            return true;
    return false;
}

bool DcTargetExclusionRegistry::IsExcluded(Player* bot, uint32 mapId, uint32 entry, bool forTank)
{
    for (DcTargetExclusionRow const& r : kRows)
        if (r.mapId == mapId && r.entry == entry)
        {
            if (forTank && !r.alsoTank)
                return false;
            return !r.inForce || r.inForce(bot);
        }
    return false;
}
