/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- Sethekk Halls (map 556) — force-summon & kill Anzu (Raven God) ---------
//
// Anzu is an OPTIONAL bonus boss with NO static spawn. In normal play he is
// summoned only by a DRUID using the Essence-Infused Moonstone (item 32449,
// heroic quest 11001) at the ante-chamber before Talon King Ikiss. Verified
// from instance_sethekk_halls.cpp / boss_anzu.cpp / smart_scripts:
//
//   * The moonstone casts a spell whose effect is SPELL_EFFECT_SEND_EVENT with
//     MiscValue 14797. The core then calls instance->ProcessEvent(target,14797).
//   * instance_sethekk_halls::ProcessEvent has NO IsHeroic()/difficulty gate:
//         if (eventId == 14797)
//             if (!GetCreature(DATA_VOICE_OF_THE_RAVEN_GOD) && GetBossState(DATA_ANZU) != DONE)
//                 instance->SummonCreature(NPC_VOICE_OF_THE_RAVEN_GOD, (-88.02,288.18,75.2));
//     At the CORE level "heroic-only" is enforced purely by item/quest
//     availability, NOT by the encounter, so a direct ProcessEvent(14797) would
//     summon Anzu on any difficulty. WE restore the blizzlike gate ourselves:
//     the event carries .HeroicOnly() (and its roster anchor gate HeroicOnly), so
//     the DriveAnzuSummon hook (id 7) only ever pokes send-event 14797 on a
//     HEROIC run — no druid, no item, no quest, no heroic key needed there. On a
//     normal run the event never fires and Anzu never surfaces. The Voice's
//     SmartAI (rows 24769-24770, event_flags 513/512 — NO DIFFICULTY_0..3 bits
//     set) would run on all difficulties, but we simply never trigger it on
//     normal.
//   * Voice (21851, TempSummon) runs action list 2185100: ~40s of theatrics
//     (the bulk is a 24s camera-shake at id6), then id9 summons Anzu (23035) at
//     (-87.61,287.84,26.5), then despawns the portals and itself.
//   * Anzu (23035, TempSummon, boss_anzu : BossAI(DATA_ANZU)) spawns
//     NON_ATTACKABLE + Shadowform; its ~16s intro then clears the flags and
//     calls SetInCombatWithZone() — which force-pulls the whole party into the
//     fight with no explicit pull needed. So ~56s poke -> fightable Anzu.
//
// WHY THE ARCATRAZ (Skyriss) SHAPE: Anzu is a TempSummon, so the spawn-store
// scans behind BossSpawnIndex/FindLiveCreatureOnMap cannot see him — adding him
// as a boss row would deadlock the engage gate on live==0. He is therefore NOT
// a boss row; the event reaches him with FindNearestCreature (a grid scan that
// DOES see TempSummons), and the roster carries only an Objective anchor for
// ordering. Cf. ArcatrazEvents.cpp (Harbinger Skyriss) and the Hellfire /
// Vazruden pattern.
//
// PRE-CLEAR: the room is swept trash-free (ClearRadius) BEFORE the poke. Anzu
// SetInCombatWithZone()s the instant it goes live, so any Sethekk trash left
// standing in the ante-chamber would pile straight into the boss fight.
//
// WHY Persistent + Optional:
//   * Persistent — the ~40s summon and the fight span several combat/non-combat
//     Drive gaps; without it the executor rewinds stepIndex to 0 on every gap
//     and re-pokes forever.
//   * Optional — Anzu is a BONUS boss; Ikiss is the real objective. A summon
//     that fails or a fight that times out must skip straight to Ikiss rather
//     than stall the clear. (This is the one departure from Arcatraz, where
//     Mellichar IS the final boss and is not optional.)
//
// WIPE: BossAI::_Reset despawns Anzu on a wipe. As with Skyriss, the
// KillCreatureEngage gate then reads "no live Anzu" and could latch complete
// without the kill — but the encounter bit is not set, so a corpse-run back
// re-drives the objective, the hook re-pokes (no Voice / no Anzu / not DONE),
// and Anzu re-summons. Optional means even a persistent failure just advances
// to Ikiss. Acceptable.

namespace
{
    constexpr uint32 SH_MAP            = 556;
    constexpr uint32 SH_ANZU           = 23035;  // TempSummon boss (grid-search only)
    constexpr uint32 SH_ANZU_ENCOUNTER = 1;      // DATA_ANZU — sorts Syth(0) < Anzu < Ikiss(2)
    constexpr uint32 SH_ANZU_SUMMON_HOOK = 7;    // ObjectiveHookRegistry id (DriveAnzuSummon)

    // The ante-chamber floor where Anzu lands and the fight happens; the party
    // parks here to summon. Anzu itself SetInCombatWithZone()s once it's live,
    // so exact placement only matters for being present when the theatrics fire.
    constexpr float SH_CHAMBER_X = -88.0f;
    constexpr float SH_CHAMBER_Y = 288.0f;
    constexpr float SH_CHAMBER_Z = 26.5f;

    // The room MUST be trash-free before the summon: once Anzu goes live it
    // SetInCombatWithZone()s, so any surviving Sethekk trash would pile into the
    // boss fight. The floor trash (Talon Lords 18321, Prophets 18325, Shamans
    // 18326, Time-Lost 18319/18320, Avian Warhawk 21904) sits at z ~26-27.5 and
    // spreads to ~44yd from the summon centre; radius 48 covers it. zBand 12
    // (z 14.5..38.5) keeps the clear on the floor and OFF the Avian Flyers /
    // Invis Raven God trigger ring at z ~41-84 (unreachable, and the triggers
    // aren't hostile anyway) so ClearRadius can't stall chasing a flyer. Well
    // clear of Ikiss (44.7,287 — 132yd away) and of the lower Syth corridor.
    constexpr float SH_CLEAR_RADIUS = 48.0f;
    constexpr float SH_CLEAR_ZBAND  = 12.0f;

    // The Objective's arrive radius must be >= the ClearRadius: while ClearRadius
    // drives the tank around the room chasing trash, a smaller arrive radius lets
    // boss-nav decide the tank has "left" the anchor and haul it back, thrashing
    // against the clear (the DireMaul/Stratholme lesson). 55 > 48 with margin.
    constexpr float SH_ARRIVE_RADIUS = 55.0f;

    // Poke -> Anzu-spawn is ~40s of theatrics; 120s covers it with margin for a
    // post-wipe re-poke without hanging a genuinely broken summon (Optional then
    // skips to Ikiss).
    constexpr uint32 SH_SUMMON_TIMEOUT = 120000;
    // The fight itself — generous but bounded so a wedged Anzu surfaces to the
    // human rather than holding the (Optional) event open indefinitely.
    constexpr uint32 SH_KILL_TIMEOUT = 180000;
}

void RegisterSethekkHallsEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(SH_MAP, /*eventId*/ 1, "Anzu (forced Raven God summon)")
                      .Anchored(/*orderIndex (doc)*/ SH_ANZU_ENCOUNTER)
                      .HeroicOnly()  // Anzu is a HEROIC-ONLY bonus boss (real WoW gates
                                     // him via the heroic quest/moonstone). Never poke
                                     // send-event 14797 on a normal run.
                      .Optional()    // bonus boss — never stall the clear to Ikiss on it
                      .Persistent()  // survives the combat/non-combat Drive gaps
                      // 1. Settle on the chamber floor before the ~40s theatrics.
                      .MoveTo(SH_CHAMBER_X, SH_CHAMBER_Y, SH_CHAMBER_Z, /*radius*/ 8.0f)
                      // 2. CLEAR THE ROOM FIRST. Anzu's SetInCombatWithZone would
                      //    otherwise drag any surviving trash into the boss fight.
                      //    Position-based (any reachable hostile in the band), so
                      //    it engages whatever the walk-in left standing.
                      .ClearRadius(SH_CHAMBER_X, SH_CHAMBER_Y, SH_CHAMBER_Z,
                                   SH_CLEAR_RADIUS, SH_CLEAR_ZBAND)
                      // 3. Fire send-event 14797 and hold through the theatrics
                      //    until Anzu is on the field. The hook stops poking the
                      //    instant Anzu exists, so it never spawns a second Voice.
                      .Custom(SH_ANZU_SUMMON_HOOK)
                          .Timeout(SH_SUMMON_TIMEOUT)
                      // 4. Engage & kill. FindNearestCreature sees the TempSummon;
                      //    Anzu's own SetInCombatWithZone pulls the party in.
                      .KillCreatureEngage(SH_ANZU, /*count*/ 1, /*searchRadius*/ 100.0f)
                          .Timeout(SH_KILL_TIMEOUT)
                      .Build());
}

void RegisterSethekkHallsRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = SH_MAP;
    // Anzu is heroic-only (see the event's .HeroicOnly() gate); keep his ordering
    // anchor off the normal-mode roster too so nothing surfaces on a normal run.
    p.gate = DcDifficultyGate::HeroicOnly;
    p.add = {
        // Objective anchor only — Anzu is NOT added as a boss row (TempSummon,
        // invisible to BossSpawnIndex). encounterIndex = DATA_ANZU (1) sorts the
        // anchor after Syth (0) and before Ikiss (2), so boss-nav delivers the
        // tank to the chamber before it heads on to Ikiss. eventId 1 links this
        // anchor to the event row above.
        MakeObjective(OBJ(1), SH_ANZU_ENCOUNTER, SH_MAP,
                      "Anzu (forced Raven God summon)",
                      SH_CHAMBER_X, SH_CHAMBER_Y, SH_CHAMBER_Z,
                      /*arriveRadius*/ SH_ARRIVE_RADIUS, /*gateEntry*/ 0,
                      /*hook*/ 0, /*eventId*/ 1),
    };
    t.push_back(std::move(p));
}
