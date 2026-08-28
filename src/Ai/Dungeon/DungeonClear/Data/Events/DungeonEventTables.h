/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONEVENTTABLES_H
#define _PLAYERBOT_DUNGEONEVENTTABLES_H

#include <unordered_map>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"

class Player;
class Creature;
class AiObjectContext;
struct BossRosterPatch;
struct DungeonWingLayout;

// Internal registration seam for the per-dungeon event tables.
//
// Each dungeon owns one .cpp in this folder that defines its event rows
// (Register<Dungeon>Events). A Conditional event's activation predicate is a
// free function defined in the SAME file, handed to the builder by pointer
// (.Conditional(&MyPredicate)) — there is no separate condition registry and no
// global id space to keep collision-free. The central DungeonEventRegistry calls
// the Register<Dungeon>Events aggregators EXPLICITLY so every per-dungeon
// translation unit stays referenced.
//
// Why explicit calls and not self-registering static initializers: the module
// compiles into a static lib, and a TU whose only output is constructor
// side-effects (with no symbol the program references) is dropped by the linker
// — its events would silently vanish. The one-big-table this replaces avoided
// that by keeping everything in a single referenced TU; the aggregator calls
// below restore the reference chain per file. (Same reason ObjectiveHookRegistry
// and friends use hardcoded tables.)
//
// Adding a dungeon:
//   1. Create <Dungeon>Events.cpp here.
//   2. Define Register<Dungeon>Events; for any Conditional event, define its
//      predicate as a static free function in that file and pass &Predicate to
//      .Conditional() (a typo is a compile error, not a silent never-fire).
//   3. Declare the appender below.
//   4. Add the call in EventTable() (DungeonEventRegistry.cpp).
//   5. If the dungeon corrects the auto-derived boss list, define its roster
//      patch as Register<Dungeon>Roster in the SAME file (using the DcRoster
//      builders in DungeonRosterBuilders.h), declare it below, and add the call
//      in PatchTable() (BossRosterRegistry.cpp). One file owns all of a
//      dungeon's clear data: event rows + conditions + roster patch.

// Shared, cross-dungeon activation predicate — external linkage so several
// dungeon files can pass &DcRoomAggroPreClearCondition to .Conditional().
// DUE while the room-trash value still has anything to clear (every
// RoomAggroRegistry boss: SM Cathedral, Scholomance Marduk & Vectus, ...).
bool DcRoomAggroPreClearCondition(Player* bot, AiObjectContext* context);

// --- event rows (one appender per dungeon) -------------------------------
void RegisterSunkenTempleEvents(std::vector<DungeonEvent>& out);
void RegisterZulFarrakEvents(std::vector<DungeonEvent>& out);
void RegisterShadowfangKeepEvents(std::vector<DungeonEvent>& out);
void RegisterScarletMonasteryEvents(std::vector<DungeonEvent>& out);
void RegisterRazorfenDownsEvents(std::vector<DungeonEvent>& out);
void RegisterBlackrockDepthsEvents(std::vector<DungeonEvent>& out);
void RegisterDeadminesEvents(std::vector<DungeonEvent>& out);
void RegisterWailingCavernsEvents(std::vector<DungeonEvent>& out);
void RegisterStratholmeEvents(std::vector<DungeonEvent>& out);
void RegisterUldamanEvents(std::vector<DungeonEvent>& out);
void RegisterScholomanceEvents(std::vector<DungeonEvent>& out);
void RegisterDireMaulEvents(std::vector<DungeonEvent>& out);
// Hellfire Ramparts (map 543) final-approach gate — see HellfireRampartsEvents.cpp
// for the measurements. Exposed so t/TestRampartsLedgeProbe can assert against the
// real navmesh that the zone-in platform lies outside the gate; the numbers are
// only meaningful together with that probe.
namespace DcHellfireRamparts
{
    // How far to scan for the Hellfire Sentries / Vazruden.
    constexpr float FINAL_APPROACH_SCAN = 45.0f;
    // Floor Z the bot must be above: the upper level the final platform sits on.
    constexpr float FINAL_APPROACH_MIN_Z = 76.0f;
}

void RegisterHellfireRampartsEvents(std::vector<DungeonEvent>& out);
void RegisterBloodFurnaceEvents(std::vector<DungeonEvent>& out);
void RegisterSlavePensEvents(std::vector<DungeonEvent>& out);
void RegisterUnderbogEvents(std::vector<DungeonEvent>& out);
void RegisterOldHillsbradEvents(std::vector<DungeonEvent>& out);
void RegisterMechanarEvents(std::vector<DungeonEvent>& out);
void RegisterShatteredHallsEvents(std::vector<DungeonEvent>& out);
void RegisterSteamvaultEvents(std::vector<DungeonEvent>& out);
void RegisterArcatrazEvents(std::vector<DungeonEvent>& out);
void RegisterSethekkHallsEvents(std::vector<DungeonEvent>& out);
void RegisterBlackMorassEvents(std::vector<DungeonEvent>& out);
// Everything in the Black Morass wave that DRAINS Medivh's shield rather than
// fighting the party: the nine trash adds (smart_scripts SMART_EVENT_RESET ->
// CAST 'Corrupt Medivh' 31326 on SELF) AND AEONUS, whose boss_aeonus::
// IsSummonedBy does the same thing in C++ and whose 37853 drains at DOUBLE the
// rate. All of them spawn REACT_DEFENSIVE and park at a home 14yd from Medivh, so
// they never aggro the party and the engage pipeline's natural pull never reaches
// them — the wave driver (ObjectiveHookRegistry hook 12) force-pulls them and
// counts them to decide when Medivh's ring needs cleaning. Excludes the Rift
// Lords / Keepers and the wave-6/12 bosses, which all fight normally.
std::vector<uint32> const& BlackMorassDrainEntries();

// The Black Morass RIFT KEEPERS — the mob each Time Rift summons 6s after it
// opens, and the ONLY thing whose death closes the rift
// (npc_time_rift::SummonedCreatureDies -> DespawnOrUnsummon). Shared with the
// wave driver (ObjectiveHookRegistry hook 12), which selects and pulls by it.
// Disjoint from BlackMorassDrainEntries() on purpose: keepers fight normally and
// never channel Corrupt, so they are never sweep targets — and the drainers never
// close a rift, so they are never selection targets.
//
// AEONUS IS NOT HERE despite being the wave-18 boss: it walks off to Medivh the
// instant it spawns (so it is never at the rift to select on) and it is not its
// rift's _riftKeeperGUID (so killing it closes nothing). It is a drainer.
std::vector<uint32> const& BlackMorassKeeperEntries();

// Wrath of the Lich King.
void RegisterUtgardeKeepEvents(std::vector<DungeonEvent>& out);
void RegisterNexusEvents(std::vector<DungeonEvent>& out);
void RegisterAzjolNerubEvents(std::vector<DungeonEvent>& out);
void RegisterAhnkahetEvents(std::vector<DungeonEvent>& out);
// Drak'Tharon Keep (map 600) — Novos' camp, shared with the hold driver
// (ObjectiveHookRegistry hook 14, HoldNovosCamp) so the camp and the keep-out it
// is placed against have exactly ONE definition. Every number is measured; the
// reasoning is in DrakTharonKeepEvents.cpp.
namespace DcDrakTharonKeep
{
    constexpr uint32 NOVOS = 26631;

    // Column-probed against the live 600 mmtiles: one walkable surface at
    // z 28.39. 19.3yd from Novos (-379.27, -737.73), 14.9yd from the Fetid Troll
    // Corpses' arrival point, 56yd from the staircase spawn trigger.
    constexpr float CAMP_X = -379.0f;
    constexpr float CAMP_Y = -757.0f;
    constexpr float CAMP_Z = 28.4f;

    // Grid-scan radius for Novos, from the activation predicate and the driver
    // alike. The chamber is ~96 x 88yd; this must cover it and the approach
    // without reaching Trollgore's arena 206yd away.
    constexpr float NOVOS_SCAN = 120.0f;

    // Keep-out around Novos while the Arcane Field (47346) is up. THIS MUST TRACK
    // the map-600 47346 row's placement `radius` in DcHazardRegistry — the driver
    // and the placement solver have to agree on one cylinder, and
    // t/TestDcHazard's DrakTharonArcaneFieldKeepOutAgreesWithTheNovosCamp pins
    // the camp against it.
    constexpr float FIELD_KEEPOUT = 14.0f;

    // Re-centring leash (the tank comes home past this UNLESS it is in melee
    // contact) and the hard leash (it comes home regardless). The hard leash is
    // sized to catch the three places phase 1 can strand a party — the staircase
    // at 56yd, ROOM_LEFT at 40yd and ROOM_RIGHT at 50yd — while leaving the
    // corpse arrival point at 14.9yd comfortably inside.
    constexpr float CAMP_LEASH = 6.0f;
    constexpr float CAMP_HARD_LEASH = 25.0f;
}

void RegisterDrakTharonKeepEvents(std::vector<DungeonEvent>& out);

// The Violet Hold (map 608) — the numbers the declarative half
// (VioletHoldEvents.cpp) and the imperative half (VioletHoldDriver.cpp) must
// agree on. Every one of them is either read straight out of violet_hold.h or
// probed against the live 608 mmtile; the reasoning is in VioletHoldEvents.cpp.
// Shared here for the same reason DcDrakTharonKeep is: a camp and the keep-out
// it is placed against need exactly ONE definition, and the gtests pin them
// against each other.
namespace DcVioletHold
{
    constexpr uint32 MAP = 608;

    // Creature entries (violet_hold.h VHCreatures).
    constexpr uint32 NPC_SINCLARI             = 30658;
    constexpr uint32 NPC_PRISON_DOOR_SEAL     = 30896;
    constexpr uint32 NPC_TELEPORTATION_PORTAL = 31011;
    constexpr uint32 NPC_CYANIGOSA            = 31134;
    constexpr uint32 NPC_ICHORON              = 29313;
    constexpr uint32 NPC_ICHOR_GLOBULE        = 29321;

    // The aura every wave add parks on the Prison Door Seal
    // (violet_hold_trashAI::CreatureStartAttackDoor -> DoCastAOE). Spell.dbc:
    // effect 6, aura 23 SPELL_AURA_PERIODIC_TRIGGER_SPELL, amplitude 3000ms,
    // DurationIndex 21 = INFINITE. spell_destroy_door_seal_aura turns each tick
    // into one ACTION_DECREASE_DOOR_HEALTH, so ONE add at the door costs the
    // 100-point gate one point every three seconds: 300s to lose the run alone,
    // 150s for two, 100s for three. That arithmetic is what sizes SEAL_DIRTY_MIN.
    constexpr uint32 SPELL_DESTROY_DOOR_SEAL  = 58040;

    // GetData ids (violet_hold.h VHData). _gateHealth has NO GetData case, which
    // is why the driver reads the seal's aura instead of the drain level.
    constexpr uint32 DATA_ENCOUNTER_STATUS    = 30;
    constexpr uint32 DATA_WAVE_COUNT          = 33;

    // GetBossState slots (violet_hold.h VHBosses). NOT DungeonEncounter bits:
    // the released pair is rolled per instance, so killing Zuramat as the first
    // prisoner sets no bit that names Zuramat. Completion rides these.
    constexpr uint32 BOSS_STATE_1ST           = 0;
    constexpr uint32 BOSS_STATE_2ND           = 1;
    constexpr uint32 BOSS_STATE_CYANIGOSA     = 2;

    // ObjectiveHookRegistry ids. Three defend hooks, not one: a Custom step is
    // handed a default-constructed DungeonBossInfo, so a shared hook cannot tell
    // which objective invoked it.
    constexpr uint32 HOOK_START               = 15;
    constexpr uint32 HOOK_DEFEND_1ST          = 16;
    constexpr uint32 HOOK_DEFEND_2ND          = 17;
    constexpr uint32 HOOK_DEFEND_CYANIGOSA    = 18;
    constexpr uint32 HOOK_DRIVE_WAVE          = 19;

    // THE DOOR CAMP — the EMERGENCY position, not the default one.
    //
    // On the flat door landing at the top of the entrance ramp (the ramp runs
    // z 38.6 at x~1869.8 up to z 44.0 at x~1861.5), straddling the funnel that
    // the last two waypoints of ALL SIX trash paths run through — (1858.95,
    // 810.05), (1860.84, 806.65), (1861.54, 804.15) and (1857.81, 796.77) all
    // lie within 7yd of it. 10.4yd inside the convergence midpoint and 32.4yd
    // from the Prison Seal itself, so the party is between the adds and the door
    // without standing on the door. Column-probed against the live 608 mmtile:
    // exactly ONE walkable surface, z 44.23.
    //
    // The first cut of this dungeon made this the party's STANDING position and
    // let the siege walk to it. That reading of the chokepoint is arithmetically
    // tidy and is not how the Violet Hold is played, or won: a keeper portal
    // pumps 3-4 adds every 20 seconds FOREVER and the only off-switch is 52-86yd
    // away at the rim, so a party that waits at the door fights the pump's output
    // instead of the pump and falls further behind every cycle. The party now
    // stations at the live portal (STAGE / rule 5 in VhDriveWave) and comes BACK
    // here only when something has actually reached the seal.
    constexpr float CAMP_X = 1855.0f;
    constexpr float CAMP_Y = 803.5f;
    constexpr float CAMP_Z = 44.05f;

    // THE STAGING POINT — where the party waits when no portal is open.
    //
    // The middle of the arena floor, at the foot of the entrance ramp: the core's
    // own MiddleRoomLocation, which is where Cyanigosa MoveJumps to on wave 18 and
    // 4.4yd from the wave-6/12 saboteur portal, so it is a position the encounter
    // itself treats as the centre of the fight. Column-probed against the live 608
    // mmtile: exactly ONE walkable surface, z 38.89.
    //
    // It exists because the party's job between waves is to be CLOSE TO THE NEXT
    // PORTAL, and the six rim portals average 45.5yd from here against 69.0yd from
    // the door camp (worst case 59.0 against 85.5). That is ~3.5s off every hop, on
    // a clock where a keeper portal starts pumping 30s after it opens. Every trash
    // path also runs through this half of the room on its way to the door, so
    // waiting here still meets the leftovers of the previous wave head-on.
    constexpr float STAGE_X = 1892.29f;
    constexpr float STAGE_Y = 805.70f;
    constexpr float STAGE_Z = 38.44f;

    // Where every wave add ends up: the midpoint of the two convergence points
    // (1843.71, 805.81, 44.14) — paths 0, 1, 1-alt, 2, 5 — and (1845.58, 800.68,
    // 44.10) — paths 3, 4, and also violet_hold_trashAI::EnterEvadeMode's new
    // home. The driver measures "how dirty is the seal" from here when the aura
    // read is unavailable.
    constexpr float SEAL_X = 1844.6f;
    constexpr float SEAL_Y = 803.2f;
    constexpr float SEAL_Z = 44.12f;

    // Arena centroid (mean of the six rim portal positions), used only by the
    // wave event's proximity gate, and the grid-scan radius that covers the whole
    // hold. The room is ~110yd across and the far rim portal is 86yd from the
    // camp; 200 sees all of it from anywhere the driver roams.
    constexpr float ARENA_X = 1893.1f;
    constexpr float ARENA_Y = 804.7f;
    constexpr float ARENA_SCAN = 200.0f;
    constexpr float EVENT_DUE_RANGE = 200.0f;

    // "Released" for a caged prisoner (and for Erekem's two guards): the
    // instance's StartBossEncounter clears UNIT_FLAG_NON_ATTACKABLE and
    // SetImmuneToNPC/All(false) at the moment the cell opens, so the flags ARE
    // the release latch. Also false through Ichoron's shattered-bubble window,
    // when he carries UNIT_FLAG_NOT_SELECTABLE for 15s — which is what makes the
    // driver retarget to his Ichor Globules instead of standing in drop-target
    // limbo. Defined in VioletHoldEvents.cpp.
    bool IsReleased(Creature const* c);
}

void RegisterVioletHoldEvents(std::vector<DungeonEvent>& out);
void RegisterMoltenCoreEvents(std::vector<DungeonEvent>& out);

// --- Blackwing Lair (map 469) ---------------------------------------------
// The numbers Razorgore's two halves must agree on. The declarative half (the
// event row and its activation predicate) is BlackwingLairEvents.cpp; the
// controller is Overrides/BlackwingLairDriver.cpp; the arithmetic is
// Util/DcRazorgoreDecision.h.
namespace DcBlackwingLair
{
    constexpr uint32 MAP_ID = 469;

    constexpr uint32 NPC_RAZORGORE = 12435;

    // The Orb of Domination and the thirty Black Dragon Eggs. Both are GOOBERs
    // (type 10) and both are WORLD SPAWNS, present from map load — so the driver
    // can grid-scan for them before anything has been engaged, unlike a
    // TempSummon-based encounter.
    constexpr uint32 GO_ORB_OF_DOMINATION = 177808;
    constexpr uint32 GO_BLACK_DRAGON_EGG  = 177807;
    constexpr uint32 EGG_COUNT            = 30;

    // instance_blackwing_lair's DATA_EGG_EVENT. The ONE readable progress seam
    // this encounter offers: it returns NOT_STARTED / IN_PROGRESS / SPECIAL /
    // DONE. There is deliberately no egg COUNT accessor in the instance script,
    // so "how many are left" is answered by scanning the eggs themselves.
    constexpr uint32 DATA_EGG_EVENT = 2;

    // Mind control (19832, 90s) and the charmer's lockout (23958, 60s). The
    // driver never runs a timer against either — it reads the charm and the aura
    // straight off the units, which is authoritative through a wipe, a despawn
    // and a phase flip alike.
    constexpr uint32 SPELL_MIND_CONTROL   = 19832;
    constexpr uint32 SPELL_MIND_EXHAUSTION = 23958;
    constexpr uint32 SPELL_DESTROY_EGG    = 19873;

    // The orb, and where the runner stands to take it: on the upper ledge at
    // z 413, above and clear of all eight add-spawn positions (all z 407).
    constexpr float ORB_X = -7614.83f;
    constexpr float ORB_Y = -1026.62f;
    constexpr float ORB_Z = 413.38f;

    // "Standing at the orb" — the runner clicks from here, and holds here for
    // its whole window (possession roots the charmer anyway; this only keeps it
    // from being walked off before the click).
    constexpr float ORB_STATION_RADIUS = 4.0f;

    // THE THREE MOBS ON THE ORB PLATFORM. Grethok the Controller (a level 62
    // elite caster) and two Blackwing Guardsmen stand 6-10yd from the orb, on the
    // ledge, from map load — they are not part of the encounter and nothing in
    // the instance script removes them.
    //
    // They are the reason this block exists. The runner's walk is 78yd and ends
    // ON TOP of them; the first live run sent one DPS up alone into three elites
    // and it died on the ramp without ever reaching the orb.
    //
    // GRETHOK IS THE PULL. He is not an encounter and carries no kill credit, but
    // he is what a human raid pulls to start this fight, and DC now treats him
    // that way: the roster patch makes him boss #0 of the map, so the tank brings
    // the raid to him with the ordinary pipeline (advance, muster, standoff,
    // engage) instead of forty bots racing each other up the ramp. Nothing on the
    // platform — no election, no staging, no click — happens before that pull.
    constexpr uint32 NPC_GRETHOK_THE_CONTROLLER = 12557;
    constexpr uint32 NPC_BLACKWING_GUARDSMAN    = 14456;

    // How far from the orb to look for them. The furthest of the three spawns
    // 10.0yd out; 25 covers that with room for the pull dragging one a few yards
    // without the scan reaching down to the floor pack (the nearest floor spawn
    // is 33yd out and a tier below).
    constexpr float ORB_GUARD_RADIUS = 25.0f;

    // GRETHOK'S SPAWN — and the roster anchor DC pulls him from.
    //
    // He is not a DungeonEncounter and the DBC has never heard of him, but for a
    // clear he is the boss of this room: he is what the tank pulls, and pulling
    // him starts the encounter (his formation holds Razorgore). So the roster
    // patch adds him as boss #0 of map 469 and the ORDINARY pull pipeline —
    // advance, raid muster, boss standoff, engage — brings the whole raid up to
    // him as one body. See RegisterBlackwingLairRoster.
    //
    // Read straight off `creature` (guid 84389): the two Blackwing Guardsmen
    // (84390 / 84391) stand 5-7yd either side of him at the same height.
    constexpr float GRETHOK_X = -7618.29f;
    constexpr float GRETHOK_Y = -1021.42f;
    constexpr float GRETHOK_Z = 413.56f;

    // The leader has to be in the chamber before the raid is sent up the ramp.
    // The event's own due range (200yd) is the whole approach; this is the room.
    constexpr float GUARD_CLEAR_RANGE = 100.0f;

    // WHERE THE REST OF THE RAID FIGHTS — at the foot of the orb platform, one
    // step toward the middle of the room, on the floor at z 408.87.
    //
    // The runner is rooted on the ledge for ninety seconds at a time and cannot
    // defend itself; if the raid fights wherever the pull left it, the adds that
    // pick the runner arrive unopposed and the mind control ends with its death.
    // So the raid camps between the room and the ledge.
    //
    // NOT on the ledge itself, which is the shape the first live run had and the
    // reason this exists: the platform is small (the navmesh column 5yd out from
    // the orb already has no 413 surface over it), so a raid standing on it has
    // nowhere to spread and nothing between it and the floor the adds cross.
    //
    // Column-probed against the live 469 mmtile: exactly one walkable surface
    // under it, z 408.87. It is 11.4yd (2D) / 12.3yd (3D) from the orb — inside
    // every healer's range of the runner, a couple of steps for a melee bot that
    // has to peel something off the ledge — and 46-81yd from all eight of the
    // instance's add-spawn positions, so every wave has to cross the room to
    // reach it rather than arriving on top of it.
    constexpr float CAMP_X = -7608.30f;
    constexpr float CAMP_Y = -1036.00f;
    constexpr float CAMP_Z = 408.87f;

    // How far off the camp anyone may drift before they are walked back. ONE
    // number for the whole raid — the tank used to get a longer tier of its own
    // (12 for members, 20 for the leader) and both were too tight live: bots
    // were being walked off adds they had legitimately stepped onto, which is
    // the one thing this rung must never do.
    //
    // 30 still keeps the raid in the runner's half of the chamber — the camp is
    // 12.3yd from the orb and the nearest add spawn is 46yd from the camp, so a
    // bot at the end of its leash is still between the room and the ledge — and
    // it is still a leash: "do not chase across the room" is the whole point of
    // a camp in a fight whose adds all come to you.
    //
    // A leash this wide COVERS THE ORB LEDGE (Grethok's own spawn is 18yd from
    // the camp centre), so a raid that fought the guard pull up there reads as in
    // position and is left alone. That is intended: the first add wave pulls it
    // down anyway, and nothing HOLDS it up there.
    //
    // THE COST, deliberately accepted: a bot at the FAR edge is 42yd from the
    // runner, past a healer's 40yd range, and the runner is rooted and cannot
    // come to it. The raid is one body in practice — it fights what walks in,
    // near the camp centre — so this is the far corner of the leash, not where
    // the fight happens. If runner deaths come back with the healers alive and
    // out of range, this number is the first suspect.
    constexpr float CAMP_LEASH = 30.0f;

    // How far INSIDE the leash a drifted bot is walked back to.
    //
    // The camp is a LEASH, not a point: the walk-back aims at the near EDGE of
    // it, never at the centre. Aiming at the centre is what the first live run
    // looked like — a bot crosses the whole camp inward, the fight pushes it
    // back out, and it crosses again, forever, every bot out of phase with the
    // others. Landing just inside the boundary makes the correction a step
    // instead of a lap.
    //
    // The margin IS the hysteresis, so it is neither zero nor a hair: land
    // exactly on the boundary and the next yard of drift re-arms the rung. It
    // must stay well under CAMP_LEASH — a margin that reached the leash would
    // put the hold point back at the centre and bring the lap back with it.
    constexpr float CAMP_HOLD_MARGIN = 4.0f;

    // Covers the whole chamber from anywhere in it: the eggs span ~80yd of x by
    // ~93yd of y across two tiers, and the orb sits 78yd from the boss's spawn.
    constexpr float ROOM_SCAN = 150.0f;

    // Proximity gate for the event's activation predicate — the leader must
    // actually be at the encounter, not corpse-running the entrance ramp.
    constexpr float EVENT_DUE_RANGE = 200.0f;

    // The event row and the hook that drives it. Hook ids are ONE FLAT SPACE
    // across every dungeon (see ObjectiveHookRegistry::AddHook); 15-19 are the
    // Violet Hold's.
    constexpr uint32 EVENT_RAZORGORE_ORB = 1;
    constexpr uint32 HOOK_RAZORGORE_ORB  = 20;

    // The orb platform, as the two facts both halves of the encounter ask about:
    // is any of Grethok / the two Blackwing Guardsmen still standing, and has the
    // tank PULLED them yet.
    //
    // `engaged` is what releases the orb runner. The click used to wait on the
    // platform being empty; it now waits on the pull, because the pull is the
    // only thing that has to happen first — Grethok's formation drags Razorgore
    // into the fight on first contact (groupAI 7), so from the tag onward every
    // second without the mind control is a second the raid spends damaging a boss
    // whose phase-1 death wipes it.
    //
    // Scanned from the ORB rather than from `bot`, so the answer does not change
    // with where the asker happens to be. Shared rather than duplicated because
    // TWO rungs act on it and a disagreement between them is a runner that clicks
    // an orb the leader thinks it is holding: the leader's driver
    // (Overrides/BlackwingLairDriver.cpp) uses it to decide the step, and the
    // runner's own rung (Action/DcRazorgoreActions.cpp) uses it to decide whether
    // to click on arrival or hold station.
    struct OrbGuardState
    {
        bool alive{false};    // at least one of the three is still up
        bool engaged{false};  // ...and somebody has it in combat
    };
    OrbGuardState OrbGuards(Player* bot);

    // IS THE EGG RUN HOLDING THE RAID WHERE IT STANDS?
    //
    // True on map 469 for every member (leader included) from the tick the pull
    // on Grethok lands until a tick or two after the last egg breaks — the same
    // window the camp rung arms on, asked by the rungs that would otherwise WALK
    // the raid somewhere else.
    //
    // It exists because of what the ordinary pipeline does the moment Grethok's
    // anchor clears: the next boss becomes Razorgore, and Razorgore — possessed,
    // being driven egg to egg by our own runner — is a MOVING anchor. The advance
    // then does exactly what it does for any wandering boss: it re-paths at his
    // live position every few seconds and holds at the engage range, and the
    // followers, who follow the tank, come with it. Measured on the first live
    // run of the egg phase (23:14:41 and 23:15:47, 45yd and 42yd splines issued
    // at the possessed boss, interleaved with "within engage range of Razorgore
    // the Untamed (25yd/27yd/11yd/23yd/17yd) -> holding for at-boss"): the raid
    // toured the chamber behind the boss at a fixed standoff instead of holding
    // the camp at the foot of the ledge, which is the one thing phase 1 asks of
    // it — the runner is rooted on that ledge for ninety seconds at a time.
    //
    // So during phase 1 nothing in the approach family may drive: not the route,
    // not the boss standoff, not the muster, not the engage. The raid's position
    // for the whole egg run belongs to the camp rung
    // (DungeonClearRazorgoreCampTrigger), which holds inside the leash and walks
    // a drifted bot back to its near edge.
    //
    // Bounded by construction, so it can never wedge a run: the stamp behind it
    // goes stale within ~3s of the driver stopping, by every exit the encounter
    // has — the last egg, a wipe, the event's own 10-minute timeout (it is
    // Optional and skips), `dc pause`, a dead leader.
    bool EggRunHoldsTheRaid(Player* bot);

    // IS THIS BOT HOLDING THE POSSESSION RIGHT NOW?
    //
    // Read off the bot's OWN unit fields — UNIT_FIELD_CHARM, resolved and checked
    // against Razorgore's entry — and off nothing else. That independence is the
    // whole point of it.
    //
    // The runner's rung and the raid's camp both hang off DcRunState::
    // razorDrivingMs, a stamp the LEADER refreshes on every tick its driver runs.
    // That is right for positioning (a stamp that goes stale releases the raid,
    // which is the safe direction) and WRONG for the channel: 19832 is a channel
    // on the runner's own body, and the instant the runner's rung goes inert the
    // bot's rotation comes back — a swing, a wand shot, a step out of a cone, a
    // health potion at ACTION_EMERGENCY — and ends it. Razorgore is then freed
    // mid-run, the runner eats a 60s lockout, and the raid is left holding a boss
    // it must not kill. Every reason the leader's stamp can go stale (the leader
    // dies, walks out of EVENT_DUE_RANGE, the event stops being due for a tick,
    // `dc pause`) is a reason the possession is STILL UP and still needs guarding.
    //
    // So the possession guards itself: the charm is a fact about this bot, no
    // cross-bot signal is involved, and both the rung that owns the tick and the
    // multiplier that mutes everything else read it here.
    //
    // Free everywhere else — the map compare rejects before the field is read.
    bool HoldsThePossession(Player* bot);
}

void RegisterBlackwingLairEvents(std::vector<DungeonEvent>& out);

// Every TempSummon the siege can field — the trash, the elites, the three portal
// keepers, Ichoron's globules, Xevozz's spheres and Cyanigosa. Probed by
// ALIVENESS by the wave event's activation predicate, which is sound only
// because none of them exists before the encounter creates it. The six caged
// prisoners and Erekem's guards are world spawns and are NOT here — see
// VioletHoldPrisonerEntries().
std::vector<uint32> const& VioletHoldWaveEntries();

// Portal Guardian 30660 / Portal Keeper 30695 / 30893 — the ONLY thing whose
// death closes a keeper portal and stops its 20-second, never-ending add pump
// (npc_vh_teleportation_portal kills itself once nothing is left to channel
// 58012 on). Shared with the wave driver, which selects and travels by it.
// Deliberately disjoint from the trash: keepers never walk to the door and never
// drain the seal, and no trash mob ever closes a portal.
std::vector<uint32> const& VioletHoldKeeperEntries();

// The six caged prisoners plus Erekem's two guards. World spawns, present from
// map load behind sealed cells, so they are probed with DcVioletHold::IsReleased
// (the instance clears NON_ATTACKABLE / IMMUNE_TO_PC on release) and never for
// mere aliveness — an aliveness probe on these would read true on an inert
// dungeon and hand the wave driver the tick before the party had even entered.
std::vector<uint32> const& VioletHoldPrisonerEntries();

// --- roster patches (one appender per dungeon that corrects the boss list) -
// Each relocates that dungeon's BossRosterPatch out of BossRosterRegistry.cpp
// so a dungeon's whole clear definition lives in one file. Aggregated by
// PatchTable() (BossRosterRegistry.cpp). Only dungeons that patch the derived
// roster appear here (e.g. Shadowfang Keep / Blood Furnace have events but no
// patch, so no roster appender).
void RegisterScarletMonasteryRoster(std::vector<BossRosterPatch>& t);
void RegisterScholomanceRoster(std::vector<BossRosterPatch>& t);
void RegisterSunkenTempleRoster(std::vector<BossRosterPatch>& t);
void RegisterRazorfenDownsRoster(std::vector<BossRosterPatch>& t);
void RegisterZulFarrakRoster(std::vector<BossRosterPatch>& t);
void RegisterBlackrockDepthsRoster(std::vector<BossRosterPatch>& t);
void RegisterBlackwingLairRoster(std::vector<BossRosterPatch>& t);
void RegisterDeadminesRoster(std::vector<BossRosterPatch>& t);
void RegisterWailingCavernsRoster(std::vector<BossRosterPatch>& t);
void RegisterStratholmeRoster(std::vector<BossRosterPatch>& t);
void RegisterDireMaulRoster(std::vector<BossRosterPatch>& t);
void RegisterUldamanRoster(std::vector<BossRosterPatch>& t);
void RegisterHellfireRampartsRoster(std::vector<BossRosterPatch>& t);
void RegisterSlavePensRoster(std::vector<BossRosterPatch>& t);
void RegisterUnderbogRoster(std::vector<BossRosterPatch>& t);
void RegisterOldHillsbradRoster(std::vector<BossRosterPatch>& t);
void RegisterMechanarRoster(std::vector<BossRosterPatch>& t);
void RegisterShatteredHallsRoster(std::vector<BossRosterPatch>& t);
void RegisterSteamvaultRoster(std::vector<BossRosterPatch>& t);
void RegisterArcatrazRoster(std::vector<BossRosterPatch>& t);
void RegisterSethekkHallsRoster(std::vector<BossRosterPatch>& t);
void RegisterBlackMorassRoster(std::vector<BossRosterPatch>& t);
void RegisterMaraudonRoster(std::vector<BossRosterPatch>& t);
void RegisterUtgardeKeepRoster(std::vector<BossRosterPatch>& t);
void RegisterNexusRoster(std::vector<BossRosterPatch>& t);
void RegisterAzjolNerubRoster(std::vector<BossRosterPatch>& t);
void RegisterAhnkahetRoster(std::vector<BossRosterPatch>& t);
void RegisterDrakTharonKeepRoster(std::vector<BossRosterPatch>& t);
void RegisterVioletHoldRoster(std::vector<BossRosterPatch>& t);
void RegisterMoltenCoreRoster(std::vector<BossRosterPatch>& t);

// --- wing layouts (one appender per split map) ---------------------------
// Records which boss credit-entries belong to which wing of a multi-wing map;
// aggregated by DungeonWingRegistry. Only split maps appear here. Maraudon has
// no events (wings + one roster removal) and lives in MaraudonEvents.cpp.
void RegisterDireMaulWings(std::unordered_map<uint32, DungeonWingLayout>& store);
void RegisterScarletMonasteryWings(std::unordered_map<uint32, DungeonWingLayout>& store);
void RegisterMaraudonWings(std::unordered_map<uint32, DungeonWingLayout>& store);

// --- anchor routes (one appender per dungeon that hand-authors a route) ---
// Waypoint anchors StridedPathfinder walks INSTEAD of asking the navmesh
// pathfinder for a corridor, for stretches where the mesh defeats it. These
// take no `out` parameter — they call DungeonClearRouteRegistry::Register
// directly — and are invoked from DungeonClearRouteRegistry's own one-time
// seed, for the same linkage reason as the tables above.
void RegisterAzjolNerubRoute();

#endif
