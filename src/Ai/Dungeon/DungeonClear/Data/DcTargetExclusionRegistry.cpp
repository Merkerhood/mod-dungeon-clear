/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcTargetExclusionRegistry.h"

#include "GameObject.h"
#include "InstanceScript.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Script/Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include <list>
#include <unordered_set>

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

    // Blackwing Lair: the drake hall bosses, while Broodlord stands.
    //
    // Broodlord's state comes from the instance's own boss slot rather than a
    // scan: Firemaw is the near end of a hall the raid does not reach for another
    // 340yd, so there is nothing local to look at, and BROODLORD_ENCOUNTER_INDEX
    // is authored from instance_blackwing_lair's enum rather than derived from the
    // roster (see the note on that constant). No instance script — never in a real
    // run of this map — reads as "still standing", the safe direction.
    //
    // THE SECOND CLAUSE IS AN OPERATOR ESCAPE, AND IT USED TO BE GEOMETRY. It was
    // `bot->GetPositionZ() >= 445`: below the drake hall's floor you cannot be in a
    // real fight with something standing on it, so the bar lifted once the raid was
    // up there and could not outlive its reason if an operator typed `dc skip` on
    // Broodlord. That reasoning is right about the APPROACH — anchors 6-11 run at
    // z 424.5, 24.6yd directly beneath Firemaw — and wrong about everything after
    // it, because the geometry does not separate the cases the way it looked:
    //
    //   Firemaw    (-7520.2, -1025.8, 449.1)
    //   Broodlord  (-7574.0, -1034.4, 449.3)   <- 54yd away, SAME FLOOR
    //   Chromaggus (-7515.3, -1029.6, 476.7)   <- 27yd straight up from Firemaw
    //
    // The upper suppression room and Broodlord's own standoff are z 449 too, so
    // the raid spends the entire legitimate second half of the gauntlet above the
    // threshold with the bar lifted and Firemaw 54yd away. Measured on
    // tp-20260828-132333-1: two of five raids killed him out of order from there.
    // No z threshold can work here, and no plan-view bound can either at 54yd.
    //
    // So the escape is asked directly instead: has the OPERATOR skipped Broodlord?
    // `dc skip` inserts the boss entry into DcKey::Skipped, which is exactly the
    // question the floor test was approximating. It also degrades safely — a bot
    // with no AI context reads "not skipped", keeping the bar up.
    bool BroodlordSkipped(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;
        AiObjectContext* ctx = botAI->GetAiObjectContext();
        if (!ctx)
            return false;
        std::unordered_set<uint32> const& skipped =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::Skipped)->Get();
        return skipped.find(DcBlackwingLair::NPC_BROODLORD_LASHLAYER) != skipped.end();
    }

    bool BwlDrakeHallOutOfOrder(Player* bot)
    {
        if (!bot)
            return false;

        if (BroodlordSkipped(bot))
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
        // The window closes when Broodlord dies OR when an operator skips him —
        // so this can never block the kills it is sequencing, by either route
        // into them.
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
