/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRaidMusterDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectAccessor.h"
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
//     instakills the raid, and DC owns that guard itself, in three layers because
//     one was not enough: DcTargetExclusionRegistry bars him from the DPS pool
//     while an egg stands, DungeonClearDpsTargetValue re-runs the pick when the
//     moon raid icon short-circuits that pool (which is what actually happened —
//     tr-20260827-233058-1, boss dead eight seconds after the pull, seventeen of
//     twenty-five bots dead with him), and DungeonClearHoldFireTrigger takes him
//     back off a bot that had already acquired him. This event's job is still to
//     make phase 1 END, which is the real fix; the guards buy it the time.
//
// VAELASTRASZ THE CORRUPT (boss 2) is the map's OTHER exception, and a much
// smaller one. He is a real boss with a real kill-bit, so the roster derives him
// for free and the playerbots `bwl` strategy already fights him (fire resistance,
// positioning, Burning Adrenaline step-outs). The single thing DC owes him is the
// OPENING: he lies friendly and passive at 30% health offering a gossip, and the
// raid starts the encounter by talking to him rather than by pulling him. So one
// conditional event does the talking, gated on the raid muster having finished —
// "when the raid is ready", which for a fight whose enrage timer starts the
// instant he turns is the whole point — and the boss-engage rung holds the raid
// where it stands from the muster through the ~63s of scripted intro, because
// everything it would otherwise do to him is either impossible (he cannot be
// attacked) or actively wrong (EngageDirect force-engages a non-hostile creature,
// which here would start the encounter with the boss still friendly and the intro
// never played).
//
// The other six BWL bosses need nothing here: all eight carry kill-credit rows
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

    // DUE while Vaelastrasz is still waiting to be talked to AND the raid is
    // ready to fight him.
    //
    // Three gates, cheapest first, because this runs on every out-of-combat tick
    // of the DC leader on map 469:
    //
    //   1. the map, and the leader's proximity to his room;
    //   2. Vaelastrasz himself, alive and still bearing the gossip flag. That
    //      flag is the ONE safe "nobody has started him" latch — BeginSpeech
    //      strips it before anything else, so it cannot double-fire, and it
    //      survives a re-entered instance (a Vaelastrasz already roused and
    //      killed is simply not there);
    //   3. THE MUSTER. DcRunState::musterPhase is the raid pre-boss gate the
    //      boss-engage rung runs for every raid boss — stage the raid at the
    //      standoff, top everyone to full, run one ForceRebuff round — and
    //      Ready is its verdict. This event asks for nothing of its own: it
    //      reads the same gate every other raid boss opens behind.
    //
    // Reading the muster rather than re-deriving readiness is what makes the
    // ordering work. The gate is FALSE while the muster runs, so the engage rung
    // (which owns the muster and sits one rung below this one) keeps the tick and
    // keeps advancing it; the tick it reaches Ready this flips true, this rung
    // takes over, and the gossip lands with the raid staged, topped and buffed.
    // The muster is timeout-bounded from every phase, so it always reaches Ready
    // and this can never deadlock a run on a bot that will not eat.
    //
    // NOT gated on the encounter or on combat: there is neither. He is friendly
    // and passive until the intro ends, so this is an ordinary between-pulls
    // event that happens to open a boss.
    bool VaelastraszRouseDue(Player* bot, AiObjectContext* context)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;
        if (bot->GetExactDist2d(VAEL_X, VAEL_Y) > VAEL_DUE_RANGE)
            return false;

        VaelastraszState const vael = DcBlackwingLair::Vaelastrasz(bot);
        if (!vael.present || !vael.offersRouse)
            return false;

        if (!context)
            return false;
        DcRunState const& run = DcRun::Of(context);
        return run.musterBossEntry == NPC_VAELASTRASZ &&
               run.musterPhase ==
                   static_cast<uint8>(DcRaidMusterDecision::Phase::Ready);
    }

    // DUE while the leader is inside the Suppression Rooms corridor with Broodlord
    // still to kill and the standoff still to reach.
    //
    // Four probes, cheapest first, because this runs on every COMBAT tick of the
    // DC leader on map 469 — and on this leg the party is in combat essentially
    // without a break:
    //
    //   1. the map;
    //   2. the corridor bbox, one axis-aligned test that is false everywhere else
    //      on the map including both encounters immediately behind the gauntlet;
    //   3. the standoff — being there is what ENDS the crossing. Expressing
    //      completion as "not yet at the far end" rather than as a latch is what
    //      makes it self-resetting: a leader shoved back into the rooms re-arms the
    //      transit, which is the correct answer, and there is no flag for a wipe to
    //      leave stale;
    //   4. Broodlord's encounter bit, which reads DONE the moment he dies and is
    //      the authoritative end of this leg.
    //
    // DELIBERATELY NOT a grid scan for Broodlord himself, which is the obvious
    // fourth probe and would break the whole thing: he stands at the FAR END of
    // the crossing, 342yd from the staging point where the transit has to arm, so
    // any scan radius honest enough to be called a room scan reads "not there" for
    // the first two thirds of the leg. The bit answers the question the scan was
    // for, from anywhere, for free.
    //
    // AND NOT GATED ON COMBAT. The crossing starts from the staging point while it
    // is still quiet — the gather gate is the first thing it does — and a predicate
    // that waited for combat would arm the driver only after the raid had already
    // walked into the whelps as a column.
    //
    // Nothing here has to stand the transit down during Broodlord's own fight: no
    // conditional event without EncounterActive is offered while a raid encounter
    // is in progress, and this one deliberately does not claim that exemption.
    bool SuppressionTransitDue(Player* bot, AiObjectContext* /*context*/)
    {
        if (!bot || bot->GetMapId() != MAP_ID)
            return false;

        if (!DcBlackwingLair::InTransitCorridor(bot))
            return false;

        if (bot->GetExactDist(TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z) <= TRANSIT_END_RADIUS)
            return false;

        InstanceScript* inst = bot->GetInstanceScript();
        return !inst || inst->GetBossState(BROODLORD_ENCOUNTER_INDEX) != DONE;
    }
}

// Grethok the Controller and the two Blackwing Guardsmen who hold the orb
// platform from map load: are any of them still up, and has anybody pulled them?
// Scanned from the ORB rather than from the bot so the answer does not change
// with where the asker happens to be standing.
//
// ONE scan for both facts, because both callers want both and a second sweep of
// the ledge every tick buys nothing. `engaged` is deliberately the PACK's combat
// flag rather than the tank's target: the tag may land on a Guardsman, the pull
// may be a body pull that aggroed all three at once, and by the time anyone asks
// the question the raid is what matters, not who opened.
//
// A world spawn is present before anything is engaged, so `alive` reads true on
// an untouched instance — which is exactly right: the tank has to pull them, and
// nothing on the platform may move before he does.
DcBlackwingLair::OrbGuardState DcBlackwingLair::OrbGuards(Player* bot)
{
    OrbGuardState st;
    if (!bot)
        return st;

    GameObject* orb = bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, ROOM_SCAN);
    WorldObject const* origin = orb ? static_cast<WorldObject*>(orb)
                                    : static_cast<WorldObject*>(bot);

    static std::vector<uint32> const kGuards = { NPC_GRETHOK_THE_CONTROLLER,
                                                 NPC_BLACKWING_GUARDSMAN };
    std::list<Creature*> found;
    origin->GetCreatureListWithEntryInGrid(found, kGuards, ORB_GUARD_RADIUS);
    for (Creature* c : found)
    {
        if (!c || !c->IsAlive())
            continue;
        st.alive = true;
        if (c->IsInCombat())
        {
            st.engaged = true;
            break;  // both flags are set; nothing left to learn from the rest
        }
    }
    return st;
}

// See the header for what this answers and why the approach family has to ask
// it. One map compare on every other map; on 469 it is the camp rung's own gate,
// read through the leader (a follower asking gets the leader's answer, not its
// own default-constructed run state).
bool DcBlackwingLair::EggRunHoldsTheRaid(Player* bot)
{
    return bot && bot->GetMapId() == MAP_ID &&
           DcLeaderSignal::IsLeaderRazorgoreDriving(bot);
}

// See the header for why this asks the bot's own charm field rather than the
// leader's stamp. Order: the map compare first (one integer, and it is the answer
// on every map but one), then the charm guid (a field read on the bot itself),
// and only then a unit lookup — so the common case costs a compare and the
// possession case costs one ObjectAccessor hit per action-relevance pass.
//
// The entry check matters: UNIT_FIELD_CHARM is set for an enslaved demon and for
// any other charm a bot might hold, and muting a warlock for the duration of
// Enslave Demon would be a bug of exactly the shape this is here to prevent.
bool DcBlackwingLair::HoldsThePossession(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return false;

    ObjectGuid const charm = bot->GetCharmGUID();
    if (charm.IsEmpty())
        return false;

    Unit* held = ObjectAccessor::GetUnit(*bot, charm);
    return held && held->IsAlive() && held->GetEntry() == NPC_RAZORGORE;
}

// See the header for what the two flags mean and why they are read together.
// One grid scan; the map compare in front of it is the answer on every other
// map, so the two callers that ask this every tick (the event's activation
// predicate and the boss-engage hold) pay nothing off 469.
DcBlackwingLair::VaelastraszState DcBlackwingLair::Vaelastrasz(Player* bot)
{
    VaelastraszState st;
    if (!bot || bot->GetMapId() != MAP_ID)
        return st;

    Creature* vael = bot->FindNearestCreature(NPC_VAELASTRASZ, VAEL_SCAN, /*alive*/ true);
    if (!vael)
        return st;

    st.present = true;
    st.offersRouse = vael->IsGossip();
    st.dormant = !bot->IsHostileTo(vael);
    return st;
}

// One axis-aligned box, and the transit's real gate. See the block in
// DungeonEventTables.h for how the bounds were drawn and what they deliberately
// exclude — everything else on this map, in particular the two encounters
// immediately behind the gauntlet.
bool DcBlackwingLair::InTransitCorridor(Player* bot)
{
    if (!bot || bot->GetMapId() != MAP_ID)
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    return x >= TRANSIT_BOX_MIN_X && x <= TRANSIT_BOX_MAX_X &&
           y >= TRANSIT_BOX_MIN_Y && y <= TRANSIT_BOX_MAX_Y &&
           z >= TRANSIT_BOX_MIN_Z && z <= TRANSIT_BOX_MAX_Z;
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

    // ROUSE VAELASTRASZ — one gossip, and nothing else.
    //
    // Everything that makes the Razorgore row exotic is absent here. The party is
    // out of combat (he is friendly and passive until the intro ends), no
    // encounter is in progress (the instance only flips IN_PROGRESS when he
    // engages), and the tank does the talking, so this is the plainest shape the
    // framework has: a Conditional event with a single Gossip step, driven by the
    // ordinary out-of-combat rung.
    //
    // ONE STEP, not "gossip then wait out the intro". The RP hold belongs to the
    // boss-engage rung (DcBlackwingLair::VaelastraszState::dormant), not to a
    // Wait step here, for a mechanical reason: a conditional event's activation
    // predicate is re-evaluated every tick, and the gossip flag this one keys on
    // is stripped by BeginSpeech — so the tick after the click the event stops
    // being due and could not have driven a Wait anyway. Letting it complete on
    // the click also means the panel's folded note flips to (done) the moment the
    // raid has actually done its part.
    //
    // NOT Repeatable, and it does not need to be. The step list completes on the
    // click, which latches it; and were the click ever to be missed, the predicate
    // simply reads true again next tick (the gossip flag is still there), because
    // an unfinished step list is not latched.
    //
    // REQUIRED. If the gossip genuinely cannot be driven — the menu never
    // populates, the tank cannot reach him — the run is stuck whatever we do
    // here, and a stall names the problem for the human instead of leaving forty
    // bots standing silently in front of a sleeping dragon.
    //
    // PANEL: sorted AFTER Razorgore rather than folded into Vaelastrasz's own
    // row, which is what it visually wants, because PanelBeforeBoss is not purely
    // cosmetic despite its name. DcTargeting::HasPendingSummonEvent keys the
    // "boss the party must SUMMON" hold off panelGatesBossEntry, and setting it
    // here would stand the whole pull pipeline down within 80yd of Vaelastrasz
    // (IsHoldingForSummonEvent) — which on this map is the entire Razorgore ->
    // Vaelastrasz corridor: four Death Talon packs and seven Blackwing Warlocks
    // spawn 25-51yd from him. Vaelastrasz is a WORLD SPAWN that is already
    // standing there; nothing here summons anything, so the hold would be pure
    // regression. PanelAfterBoss carries no such second meaning, and Razorgore is
    // the anchor immediately before him, so the row lands in the same place.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_VAELASTRASZ_ROUSE, "Vaelastrasz — start the encounter")
            .Conditional(&VaelastraszRouseDue)
            .PanelAfterBoss(NPC_RAZORGORE)
            .Gossip(NPC_VAELASTRASZ, VAEL_GOSSIP_OPTION)
            .Build());

    // CROSS THE SUPPRESSION ROOMS — the leg between Vaelastrasz and Broodlord,
    // and the only content on this map DC cannot simply walk.
    //
    // ONE Custom step, for the Black Morass / Violet Hold reason: what this leg
    // needs is a standing PREFERENCE re-decided every tick — walk, or stand for the
    // pack, or stand for an elite, or stand for the disarm rung's tick — not a
    // sequence. A step list can only say "do these in order and block on each",
    // and every one of the twenty legs can be interrupted by any of the three
    // holds at any point. The staging hop and the gather gate live INSIDE the hook
    // for the same reason: they are the first two states of one controller, not
    // two steps that happen to come first.
    //
    // DRIVES IN COMBAT — the load-bearing flag, and the whole point. The ordinary
    // conditional rung stands down on IsInCombat(), which on a leg with a hundred
    // whelps inside 20yd of the route is a rung that never runs. It is the same
    // failure the flag was written for on map 269 ("the party never left combat so
    // nothing ever walked it to a portal") and the same failure that leaves this
    // leg with no driver at all (DcCombatFlag::MayDrive).
    //
    // STEPS OWN MOVEMENT — the driver delivers the leader on its own long-range
    // spline, and the at-objective hold runs BEFORE Drive: without this, last
    // tick's glide is cancelled before the hook can even see it and the raid
    // creeps one tick at a time while every log line reports a healthy spline
    // issue. (Old Hillsbrad's barrels; Black Morass's 151 attempts, 0 arrivals.)
    // It also makes a Done RETURN YIELD THE TICK, which is what lets the raid
    // fight through every hold.
    //
    // REPEATABLE — the crossing is not a thing that completes once. The condition
    // going false (the leader reaches the standoff, or leaves the corridor, or
    // Broodlord dies) is the only "done", and a leader shoved back into the rooms
    // has to re-arm cleanly.
    //
    // PERSISTENT — the step list must not be rewound by the combat gaps. On this
    // leg a "gap" is one whelp wave dying, several times a minute.
    //
    // NOT EncounterActive: no encounter is in progress here. This is the leg
    // BETWEEN two of them, which is exactly where DC is supposed to work.
    //
    // NOT Optional, and that is a deliberate difference from the Razorgore row.
    // Every hold this driver takes is watchdog-bounded from inside, so the step's
    // own ten-minute timeout can only fire if a hold's watchdog has itself failed
    // to release — a shape nothing else here can observe. Skipping quietly at that
    // point would hand the leg back to a clear that provably cannot cross it;
    // stalling names the problem for the human, who can `dc skip` if they disagree.
    //
    // PANEL: sorted AFTER Vaelastrasz rather than BEFORE Broodlord, which is what
    // it visually wants and what it must not have — the same trap the rouse event
    // above documents, and worse here. PanelBeforeBoss keys
    // DcTargeting::HasPendingSummonEvent, which treats an unlatched gating event
    // as "this boss must still be SUMMONED" and suppresses the dynamic pull within
    // 80yd of him... and a REPEATABLE event is never latched, so the suppression
    // would be permanent: the raid would arrive at Broodlord and never pull him.
    // PanelAfterBoss carries no such second meaning, and Vaelastrasz is the anchor
    // immediately before this leg, so the row lands in exactly the same place.
    out.push_back(
        EventBuilder(MAP_ID, EVENT_SUPPRESSION_TRANSIT, "Cross the Suppression Rooms")
            .Conditional(&SuppressionTransitDue)
            .Repeatable()
            .Persistent()
            // ZERO ADVANCED PULLS ACROSS THE GAUNTLET. The crossing is a transit,
            // and a camp-drag is the exact opposite of crossing: the pull's Idle
            // branch reacts to unplanned aggro by walking a fresh camp BACK along
            // the route until it finds ground clear of hostiles, which among 160
            // whelps on a 30s respawn is never nearby, so it runs out to maxDrag
            // and hauls the tank there. With the pull stood down the tank
            // face-pulls what it meets and fights it where it stands, the party
            // stays tight (the scout-lag drops with the same predicate), and the
            // transit's own elite / pack / disarm holds are left to pace the leg.
            .OwnsThePull()
            .DrivesInCombat()
            .StepsOwnMovement()
            .PanelAfterBoss(NPC_VAELASTRASZ)
            .Custom(HOOK_SUPPRESSION_TRANSIT)
                .Timeout(TRANSIT_TIMEOUT_MS)
            .Build());
}


// --- the roster ------------------------------------------------------------
//
// GRETHOK THE CONTROLLER IS BOSS #0 of this map. He carries no DungeonEncounter
// row and no kill credit — by the DBC he is trash — but he is what a human raid
// pulls to start Razorgore, and until this patch existed nothing in DC pulled him
// at all: a bespoke rung glided every bot to a staging point on the ledge the
// moment the leader came within 100yd, which is a footrace up a ramp with the
// tank at the front and the stragglers wherever the last trash pack left them.
//
// As a boss anchor he gets the whole ordinary pipeline for free — the advance
// walks the raid to him as one body, the raid muster (Boss anchors only) tops it
// off and rebuffs it, the boss standoff parks the tank just outside his aggro
// bubble, and the engage does the pull. That is the entire fix: no new movement
// code, and the pull that starts the encounter is the same pull every other boss
// in the module gets.
//
// COMPLETION borrows Razorgore's kill-bit (inheritCompletionFrom). Grethok has no
// bit of his own, and while he stands the candidate list holds him; his corpse
// drops him out on its own (present-but-dead), and Razorgore's kill covers the
// window after the corpse decays. The driver also latches the anchor cleared the
// moment the platform reads empty (BlackwingLairDriver) — that is the wipe path,
// where the corpse is long gone and the bit will not be set for another attempt.
//
// ORDER: the eight real bosses shift to 1..8 so Grethok can hold 0. Their DBC
// kill-bits are untouched (orderOverride reorders, encounterIndex completes), so
// this is pure sequencing.
void RegisterBlackwingLairRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = DcBlackwingLair::MAP_ID;

    p.add.push_back(MakeBoss(DcBlackwingLair::NPC_GRETHOK_THE_CONTROLLER,
                             DcBlackwingLair::MAP_ID, "Grethok the Controller",
                             DcBlackwingLair::GRETHOK_X, DcBlackwingLair::GRETHOK_Y,
                             DcBlackwingLair::GRETHOK_Z,
                             /*completionFrom*/ DcBlackwingLair::NPC_RAZORGORE,
                             /*orderOverride*/ 0));

    // instance_encounters 610-617, in their own order — the classic clear path.
    p.reorder = {
        { 12435, 1 },  // Razorgore the Untamed
        { 13020, 2 },  // Vaelastrasz the Corrupt
        { 12017, 3 },  // Broodlord Lashlayer
        { 11983, 4 },  // Firemaw
        { 14601, 5 },  // Ebonroc
        { 11981, 6 },  // Flamegor
        { 14020, 7 },  // Chromaggus
        { 11583, 8 },  // Nefarian
    };

    t.push_back(std::move(p));
}


// --- the Vaelastrasz -> Broodlord route ------------------------------------
//
// FORTY anchors, in two halves, and the split between them is the whole point of
// the row:
//
//   0-19   THE APPROACH. Vaelastrasz's chamber to the staging point, 317.5yd.
//          Nothing crosses a suppression room here; this is the ordinary walk the
//          clear has always made, and it is authored for one reason only — see
//          below.
//   20-39  THE CROSSING. The staging point (anchor TRANSIT_STAGE_ANCHOR_INDEX) to
//          the Broodlord standoff, 342.6yd across the two suppression rooms. This
//          is the transit's CURSOR TRACK, and BwlTransitRoute hands the driver
//          exactly this half so its anchor 0 is still staging.
//
// WHY THE APPROACH IS AUTHORED AT ALL, which is the S2043 lesson and cost four of
// five raids the leg. It used to start at the staging point, so the 162yd from
// Vaelastrasz's corpse to anchor 0 had no route — and Blackwing Lair FOLDS BACK
// OVER ITSELF: the upper suppression room sits ~41yd directly above Vaelastrasz's
// chamber, 82yd from it as the crow flies, while the staging point is 162yd away.
// The path cursor therefore projected a tank standing on Vaelastrasz's corpse onto
// anchor 19 — the STANDOFF, the far end of the crossing — and read the leg as
// nearly finished before the raid had walked a step. Resnap failed every tick
// (>45yd, `behind=true`), the rebuild re-ran every tick, the tank sat at
// `posDelta=0.00 gen=IDLE`, and where the party eventually blundered into the
// corridor bbox decided at random where the transit armed: one run of five armed
// at staging (13yd) and crossed in 9m50s; the other four armed 33-86yd away and
// took 18-24m, two of them walking clean past Broodlord's closed portcullis — the
// navmesh does not know a GameObject is shut — and killing Firemaw out of order.
//
// A polyline the party is standing ON has none of those failure modes: the cursor
// projects onto the leg it is actually walking, Resnap has something to snap to,
// and the transit arms at staging every time because the raid arrives there.
//
// Three separate consumers measure against this row and they must measure against
// the same thing: StridedPathfinder chunks the Broodlord approach along it, the
// transit driver runs its cursor down the second half, and the pack rung leashes
// every follower to the leg the leader is on.
//
// HOW IT WAS DERIVED, and how to re-derive it. Every point here is a decimation
// of the REAL Detour corridor: LongRangePathfinder's own core, run against the
// live map-469 mmtiles by t/TestBlackwingLairSuppressionRouteProbe, printed as
// an 87-point polyline and reduced (Douglas-Peucker at 2yd, then split so no
// leg exceeds ~24yd). The plan's hand-reconstructed coordinates are NOT what is
// here — that reconstruction bridged tile seams with a 5yd centroid stitch,
// which can invent a link across a railing, and a route that walks the raid into
// geometry with 160 whelps behind it is not a route. The probe re-prints the
// polyline on every run for exactly this reason: an mmaps regen that moves the
// corridor is re-authored from the print, not by hand.
//
// The certified shape, for reading the numbers against the room:
//
//   0-3    Vaelastrasz's chamber, south along its west wall at a flat z 409
//   4-5    the SWITCHBACK out of the chamber — the corridor doubles back on
//          itself and climbs 409 -> 424 in 20yd. Decimation must never cut this
//          corner: the straight line between its ends is inside the rock.
//   6-11   north up the long hall at a flat z 424.5
//   12-16  the bend northeast and the long straight, 428.5 -> 429.3
//   17-19  the hook east onto the staging shelf, 434 -> 438
//   20     THE STAGING POINT — anchor 0 of the crossing proper
//   20-22  the climb out of the Hall of the Dragonspawn, 437 -> 443 -> 441
//   23-25  south down the lower room's east wall at a flat z 440.8
//   26-29  the long diagonal southwest across the lower room, 440 -> 442
//   30-31  the TASKMASTER RAMP's foot. Six Blackwing Taskmasters stand at
//          (-7711,-1070,445) — 3.8yd off anchor 31. There is no running past
//          them, and their 600s respawn means killing them is progress.
//   32-33  over the crest into the upper room, 446 -> 451 -> 450
//   34-39  northeast up the upper room to the standoff, flat at z 449.8
//
// PIVOT_TIGHT on 4-5 and 31-33, and only there: those are the two places the
// corridor narrows to a chokepoint the raid has to file through, and a pack that
// fans out across one of them is a pack half of which is a floor behind.
void RegisterBlackwingLairRoute()
{
    using namespace DcBlackwingLair;

    DungeonClearRouteRegistry::Register(
        MAP_ID, DUNGEON_DIFFICULTY_NORMAL, NPC_BROODLORD_LASHLAYER,
        {
            // --- the approach: Vaelastrasz's chamber to the staging point ---
            { -7506.70f, -1014.10f, 408.69f },
            { -7496.43f, -1031.26f, 409.32f },
            { -7486.39f, -1048.59f, 409.32f },
            { -7482.43f, -1068.08f, 409.41f },
            // the switchback: doubles back and climbs 15yd in 20
            { -7491.60f, -1060.33f, 418.38f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },
            // NO_STOP starts HERE, not at the first band's midpoint: the flag
            // covers the leg LEAVING an anchor, and the exposure begins 3yd into
            // the leg out of this one (route 92.9yd).
            { -7497.71f, -1055.17f, 424.28f, /*doorGoEntry*/ 0,
              AnchorFlag::PIVOT_TIGHT | AnchorFlag::NO_STOP },
            // North up the long hall — and this hall is a CEILING, not a room.
            //
            // It runs directly underneath the upper suppression room and the drake
            // hall, and 24% of the approach is inside the 3D aggro radius of what
            // stands up there. Sampling this polyline every 2yd against map 469's
            // `creature` gives three overhead bands, all at z 449.3 (24.3yd up):
            //
            //   route 92.9-105.2yd  (anchors 5-6)   up to 4 Technician / Warlock
            //   route 116.1-129.6yd (anchors 6-7)   Firemaw, the known one
            //   route 160.7-200.8yd (anchors 10-12) up to SEVEN: Technician,
            //                                       Warlock, Death Talon Overseer,
            //                                       Blackwing Spellbinder
            //
            // A level 60-62 elite aggros at ~25yd measured in 3D, and anchor 11 has
            // six of them within 27yd — 1.8-20yd away in PLAN view. They flag the
            // raid through the floor, cannot path down, and evade where they stand
            // at full HP (`first contact: Blackwing Technician at 25.0yd, 0.0yd
            // from its spawn` — the mob never moved). Nothing the party does there
            // resolves them, and every second parked buys another wave.
            //
            // So the legs from anchor 5 to anchor 12 are CROSSED, not camped. The
            // pull system's own instinct here is actively harmful: the legitimate
            // same-floor pack lives at anchors 13-16, and `safe-camp: ranged
            // attacker -> requiring LOS break, maxDrag extended to 60yd` drags that
            // camp BACKWARD into the middle of the worst band, which is then where
            // the raid fights, loots and rests. All five raids of
            // tp-20260828-175353-1 ended their run inside it, wedged at seg 13/41.
            // See DcNoStopZone and [[dc-bwl-approach-overhead-trash-aggro]].
            //
            // The flag covers the leg LEAVING each anchor, so 12 is the last one
            // marked: the raid may set up again from 13, where the nearest thing
            // overhead is 37yd away and the pack in front of it is on its own floor.
            { -7509.92f, -1044.83f, 424.31f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7520.52f, -1023.30f, 424.52f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7525.81f, -1012.53f, 424.59f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7531.11f, -1001.76f, 424.59f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7528.78f,  -990.17f, 424.55f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7523.75f,  -974.98f, 424.95f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            // the bend northeast, then the long straight
            { -7537.02f,  -955.13f, 428.52f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::NO_STOP) },
            { -7549.37f,  -944.97f, 428.65f },
            { -7564.82f,  -932.26f, 428.86f },
            { -7577.17f,  -922.10f, 428.99f },
            { -7592.62f,  -909.39f, 429.32f },
            // the hook east onto the staging shelf
            { -7601.89f,  -901.77f, 434.18f },
            { -7608.06f,  -896.69f, 433.61f },
            { -7624.62f,  -907.86f, 438.58f },
            // --- the climb into the lower suppression room ---
            { TRANSIT_STAGE_X, TRANSIT_STAGE_Y, TRANSIT_STAGE_Z },
            { -7627.03f,  -926.86f, 440.63f },
            { -7623.17f,  -938.22f, 443.28f },
            // --- the lower room, south along the wall ---
            { -7627.83f,  -953.50f, 440.78f },
            { -7633.00f,  -968.64f, 440.81f },
            { -7638.17f,  -983.78f, 440.77f },
            // --- the diagonal across the lower room ---
            { -7650.86f,  -999.24f, 440.61f },
            { -7666.09f, -1017.79f, 440.77f },
            { -7678.78f, -1033.24f, 440.73f },
            { -7694.01f, -1051.79f, 441.58f },
            // --- the Taskmaster ramp: the chokepoint between the rooms ---
            { -7706.71f, -1067.25f, 445.71f },
            { -7707.85f, -1075.17f, 445.96f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },
            { -7695.20f, -1090.66f, 451.31f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },
            { -7691.20f, -1090.68f, 449.83f, /*doorGoEntry*/ 0,
              ToFlag(AnchorFlag::PIVOT_TIGHT) },
            // --- the upper room, northeast to Broodlord ---
            { -7669.63f, -1080.17f, 449.71f },
            { -7651.64f, -1071.41f, 449.71f },
            { -7633.66f, -1062.65f, 449.85f },
            { -7612.09f, -1052.15f, 449.85f },
            { -7590.51f, -1041.64f, 449.85f },
            { TRANSIT_END_X, TRANSIT_END_Y, TRANSIT_END_Z },
        });
}
