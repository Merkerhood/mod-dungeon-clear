/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

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
