/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"

#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"

#include <list>

// --- Blackwing Lair (map 469) — Razorgore the Untamed, phase 1 -------------
//
// The declarative half of the encounter DC has to orchestrate inside somebody
// else's fight. One conditional event, one Custom step, and a predicate that
// decides when the driver owns the tick. The controller is
// Overrides/BlackwingLairDriver.cpp; the arithmetic is Util/DcRazorgoreDecision.h;
// the numbers both halves share are namespace DcBlackwingLair in
// DungeonEventTables.h.
//
// WHY THIS ONE IS DIFFERENT FROM EVERY OTHER EVENT IN THE MODULE
//
//   * IT RUNS INSIDE A RAID BOSS ENCOUNTER. Everywhere else, DC events happen
//     BETWEEN fights. On a raid map DC stands down the moment an encounter goes
//     live and hands the fight to mod-playerbots' `bwl` strategy — and this
//     event is the single sanctioned exception, the one DungeonEvent::
//     encounterActive was written for. The raid fights the adds; DC works the
//     orb and the eggs; neither touches the other's job.
//   * ITS ACTOR IS NOT THE LEADER. Every other event is performed by the tank.
//     This one needs a DIFFERENT member — the orb refuses anyone with a pet, the
//     charmer is rooted for 90 seconds, and the tank is the only thing between
//     the raid and an add every four seconds. So the driver ELECTS a runner and
//     publishes it, and the runner walks itself to the orb on its own tick
//     (DungeonClearRazorgoreOrbAction). The leader's driver never pokes another
//     bot's movement — that tug-of-war is a solved-and-relearned lesson here.
//   * KILLING THE BOSS IS A WIPE. Razorgore dying in phase 1 casts 20038 and
//     instakills the raid. DC cannot prevent that (target selection during a
//     stand-down is the strategy's); the guard is
//     RaidBwlStrategy::AppendTargetExclusions upstream, which drops him from
//     every DPS pick while eggs remain. This event's job is to make phase 1 END,
//     which is the real fix.
//
// The other seven BWL bosses need nothing here: all eight carry kill-credit rows
// (instance_encounters 610-617) so BossSpawnIndex derives the roster by itself.

namespace
{
    using namespace DcBlackwingLair;

    // The whole egg run measures ~135s of driving (262yd of tour plus 30 three-
    // second casts) across two or three 90s mind-control windows, and that is the
    // healthy case — an add-heavy run with a runner dying at the orb takes
    // longer. Ten minutes bounds a genuinely broken one without ever being the
    // binding constraint on a working one. The event is Optional, so a timeout
    // SKIPS rather than stalling the run, and Repeatable, so the next tick that
    // still sees eggs simply starts it again.
    constexpr uint32 RAZORGORE_TIMEOUT_MS = 600000;

    // DUE while Razorgore is alive on his floor with eggs left to break.
    //
    // Three probes, cheapest first, because this runs on every combat tick of the
    // DC leader on map 469:
    //
    //   1. the map, and the leader's proximity to the chamber — a corpse-running
    //      leader must not be steering a boss it cannot see;
    //   2. the instance's own DATA_EGG_EVENT, which reads DONE the instant the
    //      thirtieth egg pops and is the authoritative end of phase 1. Testing it
    //      before the grid scan means the tick after the last egg costs one
    //      GetData, not a sweep of the room;
    //   3. a live Razorgore, and at least one egg still standing.
    //
    // Deliberately NOT gated on "is the raid in combat" or "is the encounter in
    // progress". The orb can and should be taken before anyone has pulled: the
    // instance starts the add waves off the FIRST EGG as readily as off the pull
    // (SetData(SPECIAL) promotes NOT_STARTED to IN_PROGRESS), so a run that gets
    // to work early simply eats fewer adds.
    //
    // A destroyed egg is gone rather than flagged: spell_egg_event Uses the
    // goober and sets its respawn a week out, so it despawns within ten seconds.
    // Counting `isSpawned() && GO_STATE_READY` therefore counts exactly the eggs
    // still to break, through a wipe and a re-entry alike.
    bool RazorgoreEggRunDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;
        if (bot->GetExactDist2d(ORB_X, ORB_Y) > EVENT_DUE_RANGE)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        if (inst && inst->GetData(DATA_EGG_EVENT) == DONE)
            return false;

        Creature* razor = bot->FindNearestCreature(NPC_RAZORGORE, ROOM_SCAN, /*alive*/ true);
        if (!razor)
            return false;

        std::list<GameObject*> eggs;
        razor->GetGameObjectListWithEntryInGrid(eggs, GO_BLACK_DRAGON_EGG, ROOM_SCAN);
        for (GameObject* egg : eggs)
            if (egg && egg->isSpawned() && egg->GetGoState() == GO_STATE_READY)
                return true;

        return false;
    }
}

void RegisterBlackwingLairEvents(std::vector<DungeonEvent>& out)
{
    // ONE Custom step, for the same reason the Violet Hold's wave driver is one:
    // what this encounter needs is a standing PREFERENCE re-decided every tick as
    // charms expire and runners die, not a sequence. A step list can only say "do
    // these in order and block on each", and every one of the ~35 discrete acts
    // here (elect, stage, click, walk, cast, ×30, ×3 windows) can be undone by
    // the next tick's world.
    //
    // ENCOUNTER ACTIVE — the load-bearing flag, and the reason it exists. Without
    // it FindDueConditionalEvent refuses this event outright the moment the raid
    // pulls, which is precisely when it has work to do.
    //
    // DRIVES IN COMBAT — the party is in combat from the first add to the last.
    // The ordinary conditional rung stands down on IsInCombat(), so without this
    // the driver would only run in the gaps between waves, of which there are
    // none.
    //
    // STEPS OWN MOVEMENT — the driver walks the POSSESSED BOSS with its own
    // splines and must not have them cancelled by the at-objective hold on the
    // next tick (the Old Hillsbrad barrel trap / the Black Morass's 151
    // attempts, 0 arrivals).
    //
    // OPTIONAL + REPEATABLE — a timeout skips instead of stalling a human's run,
    // and the next tick that still sees eggs re-fires it fresh. That is also the
    // wipe path: Razorgore respawns 30s after a phase-1 death with the instance's
    // own SetData(FAIL) having reset the field, and the event simply comes back.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_RAZORGORE_ORB, "Razorgore — orb and egg run")
            .Conditional(&RazorgoreEggRunDue)
            .Repeatable()
            .Optional()
            .DrivesInCombat()
            .EncounterActive()
            .StepsOwnMovement()
            .Custom(HOOK_RAZORGORE_ORB)
                .Timeout(RAZORGORE_TIMEOUT_MS)
            .Build());
}
