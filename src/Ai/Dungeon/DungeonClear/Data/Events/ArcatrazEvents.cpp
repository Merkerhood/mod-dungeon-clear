/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- The Arcatraz (map 552) — Warden Mellichar's stasis-pod finale ---------
//
// The last encounter is a fixed multi-wave set-piece with no static boss spawn
// at the end of it. Verified from arcatraz.cpp / instance_arcatraz.cpp:
//
//   * Warden Mellichar (20904) IS a normal spawn, on his dais at
//     (445.803, -169.007, 43.6442). He is SetImmuneToAll, zeroes all incoming
//     damage, and overrides MoveInLineOfSight / AttackStart / JustEngagedWith to
//     empty — he never fights and never loses HP.
//   * The encounter starts when a PLAYER DAMAGES HIM (DamageTaken). There is no
//     gossip, no area trigger, no clickable object. See the hook, below.
//   * Five stasis pods then open on a fixed timer, each releasing a wave:
//       wave 1  (478.3,-148.5)  RAND(20905 Blazing Trickster, 20906 Phase-Hunter)
//       wave 2  (413.3,-148.4)  20977 Millhouse Manastorm  — COSMETIC, not gated
//       wave 3  (420.2,-174.4)  RAND(20908 Akkiris Lightning-Waker,
//                                    20909 Sulfuron Magma-Thrower)
//       wave 4  (471.8,-174.6)  RAND(20910 Twilight Drakonaar,
//                                    20911 Blackwing Drakonaar)
//       wave 5  (445.8,-191.6)  20912 Harbinger Skyriss — the real boss
//     Waves 1/3/4 gate on their own death (SmartAI JustDied -> SetInstanceData);
//     wave 2 is pure theatre and the sequence does not wait for Millhouse.
//   * Killing Skyriss makes Mellichar KillSelf() and latches boss bit 3.
//
// WHY A GARRISON RATHER THAN AN INSTANCE-DATA GATE: instance_arcatraz does NOT
// implement GetData at all — it overrides only OnGameObjectCreate / SetData /
// GetGuidData / SetBossState, so GetData falls through to ZoneScript's base
// `return 0` for every id, forever. MoveToHoldUntilInstanceData (the BRD Ring of
// Law / ZulFarrak pattern) would read 0 and either never clear or clear
// instantly. The pod SetData values are forwarded straight to the pod GO and to
// Mellichar's AI and are never stored. So the wave phase is gated on Skyriss
// EXISTING instead — WaitForSpawn/garrison uses FindNearestCreature, which sees
// a TempSummon (the spawn-store scans do not).
//
// WHERE THE PARTY STANDS: (445.9, -161.5, 42.56), the centroid of the four wave
// spawn corners. Each corner is ~30-36yd away, so no wave lands on top of the
// party, and it is well inside the encounter's 100yd leash
// (EVENT_WARDEN_CHECK_PLAYERS evades the whole fight within 1s if no player is
// within 100yd of Mellichar). It is also ~11yd off Mellichar's dais, so the
// party is not stacked underneath him during the intro.
//
// WIPE HANDLING: BossAI::_Reset despawns every wave mob and Skyriss, re-closes
// all five pods, re-raises the shield and puts the boss state back to
// NOT_STARTED — but does NOT despawn Mellichar. A wipe DURING THE WAVES recovers
// cleanly: the hook keeps the poke live for the whole phase, so a party that
// corpse-runs back sees NOT_STARTED and restarts the sequence.
//
// KNOWN GAP — a wipe during the SKYRISS FIGHT does not. Step 3's KillCreature
// gate completes when no live Skyriss remains in range, and a reset despawns
// him, so the gate reads Done and the event latches complete without the kill.
// Mellichar's boss row is still unfinished, so the run then drives the party
// back to his dais and the boss-engage machinery re-pokes him there (his
// DamageTaken is the trigger, and engaging him IS damage) — the waves rerun,
// fought from the dais rather than the centroid. Sloppy but not stuck. Closing
// it properly needs a completion gate on encounter bit 3 rather than on the
// creature's absence, which the KillCreature step does not offer today.
//
// The two Containment Core security fields (184318 / 184319, at x~199.9) need no
// event rows: they are plain DOOR_TYPE_PASSAGE gates that open permanently when
// Soccothrates and Dalliah die, which ordinary boss ordering already handles.
//
// SEPARATELY: the Arcatraz Sentinels (20869) are handled by DcHazardRegistry (not
// an event). They wake AGGRESSIVE at 40% HP and are fought normally; the danger is
// what happens AFTER. (1) A dormant one pulses 563-937/s in 15yd; pull/camp/route
// avoidance keeps bots from loitering in it. (2) The run-wiper: on death the
// Sentinel summons the "Destroyed Sentinel" (21761) at the corpse — NOT_SELECTABLE,
// so unkillable, carrying the same 15yd/1s pulse until it despawns. The party is
// standing right on it after the kill; DungeonClearHazardVacate drives every bot
// out of the pulse (both engines, since combat usually drops). The live one's <=10%
// "Explode" (36719) needs no handling — it self-stuns for the wind-up, so the party
// bursts it down before it detonates. See DcHazardRegistry.h.

namespace
{
    // The wave arena floor centroid — where the whole finale is fought.
    constexpr float ARC_ARENA_X = 445.9f;
    constexpr float ARC_ARENA_Y = -161.5f;
    constexpr float ARC_ARENA_Z = 42.56f;

    constexpr uint32 ARC_SKYRISS = 20912;

    // ObjectiveHookRegistry id 6 — pokes Mellichar and holds through the waves.
    constexpr uint32 ARC_MELLICHAR_WAVES_HOOK = 6;

    // Mellichar's DBC encounter slot; the objective shares it so the roster's
    // Objective-before-Boss tie-break sorts it immediately ahead of him.
    constexpr uint32 ARC_MELLICHAR_ENCOUNTER = 3;

    // The scripted intro alone is ~2 minutes of hard-coded delays before Skyriss
    // is released, and three of the four waves gate on a kill in between. 300s is
    // ~2.5x the realistic worst case without being so loose that a genuinely
    // wedged run sits here instead of surfacing to the human.
    constexpr uint32 ARC_WAVES_TIMEOUT = 300000;
}

void RegisterArcatrazEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(552, 1, "Warden Mellichar (stasis pod waves)")
                      .Anchored(/*orderIndex (doc)*/ 3)
                      // Every wave is a fight, and the executor only Drives from
                      // the NON-COMBAT strategy — so each wave produces a Drive
                      // gap longer than EventStaleGapMs. Without Persistent the
                      // step index rewinds to 0 on every gap and the event
                      // restarts forever. (Also required by the persistence lint:
                      // this event holds two rewind-hazard steps.)
                      .Persistent()
                      // 1. Settle on the arena floor, clear of all four corners.
                      .MoveTo(ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z, /*radius*/ 6.0f)
                      // 2. Own the whole wave phase: poke Mellichar to open the
                      //    pods, garrison the centroid between waves, and finish
                      //    when Skyriss is released. Combat AI fights the waves
                      //    as they arrive.
                      //
                      //    ONE Custom step rather than Custom(poke) +
                      //    MoveToHoldUntilSpawn(Skyriss), because this event must
                      //    be Persistent and the executor never rewinds a
                      //    persistent event's stepIndex — a poke step that has
                      //    already returned Done could not re-fire after a wipe,
                      //    and the hold would then wait out its whole timeout on
                      //    a Skyriss nobody was left to release. See the hook.
                      .Custom(ARC_MELLICHAR_WAVES_HOOK)
                          .Timeout(ARC_WAVES_TIMEOUT)
                      // 3. Kill him. Mellichar KillSelf()s the instant Skyriss
                      //    dies, so his boss row completes with this step.
                      .KillCreatureEngage(ARC_SKYRISS, /*count*/ 1, /*searchRadius*/ 100.0f)
                      .Build());
}

void RegisterArcatrazRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = 552;
    p.add = {
        // Mellichar's own boss row is KEPT, not removed: he is a real spawn with
        // correct coords and his kill-bit (3) latches on the win. The objective
        // shares his encounter index and sorts ahead of him, so by the time his
        // row is considered the event has already killed Skyriss and he is gone.
        //
        // Skyriss is deliberately NOT added as a boss row. He is a TempSummon, so
        // BossSpawnIndex cannot see him and the spawn-store scans behind
        // FindLiveCreatureOnMap would read live=0 and deadlock the engage gate.
        // The event reaches him with FindNearestCreature instead, which does see
        // TempSummons — the Hellfire Ramparts / Vazruden pattern.
        MakeObjective(OBJ(1), ARC_MELLICHAR_ENCOUNTER, 552,
                      "Warden Mellichar (stasis pod waves)",
                      ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z,
                      /*arriveRadius*/ 12.0f, /*gateEntry*/ 0,
                      /*hook*/ 0, /*eventId*/ 1),
    };
    t.push_back(std::move(p));
}
