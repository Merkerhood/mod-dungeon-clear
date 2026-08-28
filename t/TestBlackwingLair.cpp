/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>
#include <vector>

#include "Position.h"

#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DcRelevance.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"

// Blackwing Lair (map 469) — Razorgore's orb and egg run.
//
// Two halves, both lintable without a world: the authored EVENT (whose flag set
// is the whole reason the encounter can be driven at all) and the pure KERNEL
// that decides what the driver does each tick.

namespace
{
    using namespace DcBlackwingLair;

    // A View in the middle of a healthy egg run: possessed, boss idle, an egg
    // elected and out of range. Every test below perturbs one field of it, so the
    // thing under test is always the single difference.
    DcRazorgore::View Driving()
    {
        DcRazorgore::View v;
        v.eventDone     = false;
        v.bossAlive     = true;
        v.eggsRemaining = 17;
        v.bossCharmed   = true;
        v.bossCasting   = false;
        v.orbGuardsAlive = false;
        v.orbGuardsEngaged = false;
        v.haveRunner    = true;
        v.runnerAtOrb   = true;
        v.runnerCanClick = true;
        v.haveEgg       = true;
        v.bossToEgg     = 30.0f;
        return v;
    }

    DcRazorgore::RunnerCandidate Bot(uint32 guid)
    {
        DcRazorgore::RunnerCandidate c;
        c.guidLow = guid;
        c.alive = true;
        c.isBot = true;
        return c;
    }
}

// --- the authored event ---------------------------------------------------

TEST(DungeonEventBlackwingLairTest, RazorgoreEventIsAnEncounterActiveCombatDriver)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(MAP_ID, EVENT_RAZORGORE_ORB);
    ASSERT_NE(ev, nullptr) << "Blackwing Lair (469) event 1 (Razorgore orb) is missing";

    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition)) << "the egg-run gate predicate must be bound";

    // THE flag. On a raid map DcBossStandDown makes every DC behaviour inert the
    // moment an encounter goes live, and FindDueConditionalEvent refuses any event
    // without this one outright — so dropping it does not degrade the encounter,
    // it silently removes it exactly when it has work to do.
    EXPECT_TRUE(ev->encounterActive)
        << "the orb event must be EncounterActive: it is the ONE exemption to the raid "
           "boss stand-down, and without it the driver never runs inside the fight";

    // The adds never stop until the last egg pops, so the party is in combat for
    // the whole run and a non-combat-only driver would never get a tick.
    EXPECT_TRUE(ev->drivesInCombat) << "the orb event must DrivesInCombat (continuous adds)";
    // The driver issues the POSSESSED BOSS's splines; the per-tick objective hold
    // would cancel them on the next tick.
    EXPECT_TRUE(ev->stepsOwnMovement) << "the orb event must StepsOwnMovement";
    // A phase-1 wipe soft-resets the encounter (the boss respawns in 30s and the
    // instance clears the field), so the event has to be able to come back.
    EXPECT_TRUE(ev->repeatable) << "the orb event must be Repeatable (the wipe path)";
    EXPECT_FALSE(ev->required) << "the orb event must be Optional (a timeout re-fires it)";

    // ONE Custom step: what this encounter needs is a preference re-decided every
    // tick as charms expire and runners die, not a sequence of gates.
    ASSERT_EQ(ev->steps.size(), 1u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Custom);
    EXPECT_EQ(ev->steps[0].hookId, HOOK_RAZORGORE_ORB);
    EXPECT_TRUE(ObjectiveHookRegistry::Has(HOOK_RAZORGORE_ORB))
        << "hook " << HOOK_RAZORGORE_ORB << " (DriveRazorgoreOrb) must be registered";
}

TEST(DungeonEventBlackwingLairTest, AuthoredIdsMatchTheWorldData)
{
    // Verified against the live world DB and Spell.dbc; a typo here is invisible
    // at runtime (a scan that finds nothing simply never fires).
    EXPECT_EQ(MAP_ID, 469u);
    EXPECT_EQ(NPC_RAZORGORE, 12435u);
    EXPECT_EQ(GO_ORB_OF_DOMINATION, 177808u);
    EXPECT_EQ(GO_BLACK_DRAGON_EGG, 177807u);
    EXPECT_EQ(EGG_COUNT, 30u);
    EXPECT_EQ(SPELL_MIND_CONTROL, 19832u);
    EXPECT_EQ(SPELL_MIND_EXHAUSTION, 23958u);
    EXPECT_EQ(SPELL_DESTROY_EGG, 19873u);

    // The commit band must stay well inside the spell's 10yd range: 19873 carries
    // interrupt flags 31, so arriving "just barely" in range is how a cast gets
    // eaten by the last half-yard of slide.
    EXPECT_LT(DcRazorgore::kCastBand, 10.0f);
    // ...and outside the re-path epsilon, or the driver would oscillate between
    // "close enough to cast" and "re-issue the spline".
    EXPECT_GT(DcRazorgore::kCastBand, DcRazorgore::kRepathEpsilon);
}

TEST(DungeonEventBlackwingLairTest, TheRaidCampsOffTheLedgeAndInHealRangeOfTheRunner)
{
    // The fix from the first live run: the orb and the egg run worked, and the
    // raid fought wherever the pull had left it, so every add that picked the
    // rooted runner arrived unopposed.
    //
    // The camp is on the FLOOR (column-probed: one walkable surface at 408.87),
    // not on the orb platform at 413 — a raid on a small ledge has nowhere to
    // spread and nothing between it and the floor the adds cross.
    EXPECT_LT(CAMP_Z, ORB_Z - 3.0f) << "the camp must be BELOW the orb platform";

    // Close enough that every healer covers the runner (heal range is 40yd) and a
    // melee bot can peel something off the ledge in a couple of steps...
    float const dx = CAMP_X - ORB_X, dy = CAMP_Y - ORB_Y, dz = CAMP_Z - ORB_Z;
    float const toOrb = std::sqrt(dx * dx + dy * dy + dz * dz);
    EXPECT_LT(toOrb, 20.0f) << "camp is too far from the orb to protect the runner";
    // ...and far enough out that the raid is not standing on the platform itself.
    EXPECT_GT(toOrb, 8.0f) << "camp has crept back onto the ledge";

    // Every add-spawn position the instance uses is a room's width away, so a wave
    // has to cross to the raid rather than landing on top of it.
    Position const spawns[] = {
        {-7661.207520f, -1043.268188f, 407.199554f}, {-7644.145020f, -1065.628052f, 407.204956f},
        {-7624.260742f, -1095.196899f, 407.205017f}, {-7608.501953f, -1116.077271f, 407.199921f},
        {-7531.841797f, -1063.765381f, 407.199615f}, {-7547.319336f, -1040.971924f, 407.205078f},
        {-7568.547852f, -1013.112488f, 407.204926f}, {-7584.175781f, -989.669128f, 407.199585f},
    };
    for (Position const& p : spawns)
        EXPECT_GT(p.GetExactDist(CAMP_X, CAMP_Y, CAMP_Z), 40.0f)
            << "add spawn (" << p.GetPositionX() << ", " << p.GetPositionY()
            << ") is close enough to the camp that its wave arrives on top of the raid";

    // ONE leash for the whole raid (the tank's separate tier is gone — 30 is
    // loose enough for anyone to step onto an add that reached the healers). It
    // is still a leash and not a free rein, and the bound that decides the number
    // is the RUNNER: it is rooted on the ledge and cannot come to a healer, so a
    // bot at the far edge of the camp must not be hopelessly out of range of it.
    EXPECT_GT(CAMP_LEASH, 0.0f);
    EXPECT_LT(CAMP_LEASH + toOrb, 45.0f)
        << "a bot at the far edge of the camp is out of any reach of the runner — "
           "the leash has stopped being a camp";

    // WHAT A LEASH THIS WIDE DECIDES: GRETHOK'S OWN SPAWN is inside it, so the
    // guard fight the tank pulls happens within the camp and the raid is not
    // walked off the pack it is killing on the first tick of the egg run. The
    // camp arms with that pull (razorDrivingMs), so this is not a nicety — a
    // leash that excluded the platform would fight the pull for every melee bot
    // in the raid. Re-read DungeonClearRazorgoreCampTrigger before "fixing" the
    // number.
    float const sdx = GRETHOK_X - CAMP_X, sdy = GRETHOK_Y - CAMP_Y,
                sdz = GRETHOK_Z - CAMP_Z;
    EXPECT_LT(std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz), CAMP_LEASH)
        << "the camp yanks the raid off the guard pull it was just sent to make";

    // THE PING-PONG THIS ENCODES: the walk-back aims at the camp's near edge,
    // CAMP_HOLD_MARGIN inside the leash, so a bot the fight pushes out takes one
    // step back and holds. A margin of zero puts the hold point ON the boundary
    // and the next yard of drift re-arms the rung; a margin that reached the
    // leash puts it back at the centre and every correction is a lap across the
    // whole camp again. Both ends are the same bug, so both are asserted.
    EXPECT_GT(CAMP_HOLD_MARGIN, 0.0f)
        << "no margin is no hysteresis — the rung re-arms the moment it releases";
    EXPECT_LT(CAMP_HOLD_MARGIN, CAMP_LEASH)
        << "the margin has swallowed the tightest leash, so the walk-back aims at "
           "the camp centre again and the ping-pong is back";

    // And the step itself has to be a step: at the tightest leash the correction
    // is CAMP_HOLD_MARGIN yards, which must stay small next to the camp's own
    // radius or "hold at the edge" is indistinguishable from "walk to the middle".
    EXPECT_LT(CAMP_HOLD_MARGIN, CAMP_LEASH * 0.5f)
        << "the walk-back crosses more than half the camp — that is a lap, not a step";
}

TEST(DungeonEventBlackwingLairTest, OrbRungOutranksTheRaidStrategy)
{
    // mod-playerbots' `bwl` strategy nodes sit at ACTION_RAID (60) and
    // ACTION_RAID+1 (61) — and one of them, `bwl razorgore avoid aoe`, walks bots
    // out of the boss's frontal cone. It would walk the orb runner off the ledge
    // every tick of its trip if this rung did not outrank it.
    EXPECT_GT(DcRel::RazorgoreOrb, 61.0f);
    // Below the phantom-combat hatch, which must always win when it legitimately
    // fires.
    EXPECT_LT(DcRel::RazorgoreOrb, DcRel::BreakStuckCombat);

    // The raid's camp sits between the two: above the strategy nodes it has to
    // beat (`bwl razorgore avoid aoe` would walk bots straight out of the camp),
    // below the runner, because a bot that is somehow both has the more urgent job
    // at the orb.
    EXPECT_GT(DcRel::RazorgoreCamp, 61.0f);
    EXPECT_LT(DcRel::RazorgoreCamp, DcRel::RazorgoreOrb);
}

// --- the kernel: what the driver does this tick ---------------------------

TEST(DungeonEventBlackwingLairTest, CompletionOutranksEverything)
{
    // The dangerous tick is the one right after the thirtieth egg: the raid is now
    // trying to KILL Razorgore, and a driver that re-elected a runner and re-took
    // the orb there would mind-control the boss out of its own kill.
    DcRazorgore::View v = Driving();
    v.eventDone = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);

    v = Driving();
    v.eggsRemaining = 0;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);

    v = Driving();
    v.bossAlive = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);
}

TEST(DungeonEventBlackwingLairTest, PossessedBossWalksThenCasts)
{
    DcRazorgore::View v = Driving();
    v.bossToEgg = DcRazorgore::kCastBand + 0.1f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::MoveBoss);

    v.bossToEgg = DcRazorgore::kCastBand - 0.1f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::CastEgg);
}

TEST(DungeonEventBlackwingLairTest, ACastInFlightYieldsTheTick)
{
    // Destroy Egg is a 3s cast — thirty ticks. Claiming them would starve the
    // raid's combat engine for the whole egg run, which is how parties wipe to the
    // add wave while the driver is doing everything right.
    DcRazorgore::View v = Driving();
    v.bossCasting = true;
    v.bossToEgg = 1.0f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitCast);
}

TEST(DungeonEventBlackwingLairTest, EveryEggSkippedAsksForAFreshPass)
{
    // Not Done: eggs remain, they are just all on the skip list this pass. Idle is
    // the caller's cue to clear it and try again — a skip is "not now", never
    // "never", and treating it as completion would end phase 1 with eggs standing.
    DcRazorgore::View v = Driving();
    v.haveEgg = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Idle);
}

TEST(DungeonEventBlackwingLairTest, TheMindControlCycleRotatesRunners)
{
    // Window over (90s expired, or the runner died): back to electing.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // Elected but still walking the 78yd to the ledge.
    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::StageRunner);

    // Standing on it and legal: take the orb.
    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);

    // Standing on it and NOT legal — the 60s Mind Exhaustion from its own last
    // window, or a pet it resummoned on the way. Someone else goes; waiting out a
    // lockout at the orb would cost the run a whole window.
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);
}

// --- the kernel: who takes the orb ----------------------------------------

TEST(DungeonEventBlackwingLairTest, RunnerElectionEnforcesTheOrbsOwnRefusals)
{
    // go_orb_of_domination::GossipHello refuses a user with a pet or with Mind
    // Exhaustion up, and a corpse cannot click anything. A real player has no
    // PlayerbotAI for the rung to run on. The leader is the main tank and is the
    // only thing between the raid and an add every four seconds.
    std::vector<DcRazorgore::RunnerCandidate> pool;
    DcRazorgore::RunnerCandidate dead = Bot(1);      dead.alive = false;
    DcRazorgore::RunnerCandidate human = Bot(2);     human.isBot = false;
    DcRazorgore::RunnerCandidate pet = Bot(3);       pet.hasPet = true;
    DcRazorgore::RunnerCandidate tired = Bot(4);     tired.exhausted = true;
    DcRazorgore::RunnerCandidate leader = Bot(5);    leader.isLeader = true;
    pool = {dead, human, pet, tired, leader};
    EXPECT_EQ(DcRazorgore::SelectRunner(pool), -1)
        << "every candidate is barred; the driver must report no runner, not pick one";

    // One legal body is enough, whatever its role.
    DcRazorgore::RunnerCandidate healer = Bot(6);    healer.isHealer = true;
    pool.push_back(healer);
    EXPECT_EQ(DcRazorgore::SelectRunner(pool), 5);
}

TEST(DungeonEventBlackwingLairTest, RunnerElectionPrefersARangedDps)
{
    // The orb is on the upper ledge, 78yd from the boss and above every add spawn,
    // and the charmer is rooted there for 90 seconds. A ranged bot keeps DPSing
    // through its window; a melee bot is simply out of the fight, and a healer
    // parked there is how the raid dies to the adds.
    DcRazorgore::RunnerCandidate healer = Bot(1);  healer.isHealer = true;
    DcRazorgore::RunnerCandidate offtank = Bot(2); offtank.isTank = true;
    DcRazorgore::RunnerCandidate melee = Bot(3);
    DcRazorgore::RunnerCandidate ranged = Bot(4);  ranged.isRanged = true;

    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank, melee, ranged}), 3);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank, melee}), 2);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer, offtank}), 1);
    EXPECT_EQ(DcRazorgore::SelectRunner({healer}), 0);
}

TEST(DungeonEventBlackwingLairTest, RunnerElectionIsStableAcrossContexts)
{
    // Every member computes the election independently (the leader publishes it,
    // but the runner's own rung re-derives eligibility); a tie broken by anything
    // but the lowest GUID would let two bots disagree about who is going.
    DcRazorgore::RunnerCandidate a = Bot(70); a.isRanged = true;
    DcRazorgore::RunnerCandidate b = Bot(12); b.isRanged = true;
    DcRazorgore::RunnerCandidate c = Bot(41); c.isRanged = true;
    EXPECT_EQ(DcRazorgore::SelectRunner({a, b, c}), 1);
    EXPECT_EQ(DcRazorgore::SelectRunner({c, a, b}), 2);
    EXPECT_EQ(DcRazorgore::SelectRunner({b, c, a}), 0);
}

// --- the phase-1 damage guard --------------------------------------------

TEST(DungeonEventBlackwingLairTest, NothingTouchesThePlatformBeforeTheTankPullsIt)
{
    // THE ORDERING THIS FILE EXISTS TO PIN. Grethok is a boss anchor, so the pull
    // is the tank's and the whole raid comes with it (advance, muster, standoff,
    // engage). Until that pull lands, the kernel must not elect anybody, must not
    // stage anybody, and must not click: each of those puts one DPS bot on a
    // ledge 78yd ahead of its raid, next to three level-62 elites. The shape that
    // staged "with the raid" needed a rung that glided forty bots at the ledge to
    // make it true — which is the footrace this replaced.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.orbGuardsAlive = true;
    v.orbGuardsEngaged = false;
    v.bossEngaged = false;

    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull)
        << "an election before the pull arms a runner's rung and sends it up alone";

    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull)
        << "the staging walk ends on top of three elites nobody is fighting";

    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::WaitPull);

    // An empty platform is the other way out of the wait — a re-entry, or a raid
    // that killed the guards on some earlier attempt.
    v.orbGuardsAlive = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, TheRunnerTakesOverTheMomentGrethokIsEngaged)
{
    // The other half of the same contract, and the reason the wait is a wait and
    // not a "clear the platform first": from the tag onward, Razorgore is already
    // in the fight (creature_formations, groupAI 7), the add pump is already
    // running (the instance promotes NOT_STARTED on the pull, not on the orb), and
    // the raid is already damaging a boss whose phase-1 death casts 20038 on all
    // forty of them. Waiting for the guards to die on top of that is pure loss.
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.orbGuardsAlive = true;
    v.orbGuardsEngaged = true;   // the tank has Grethok
    v.bossEngaged = false;       // ...and the formation has not dragged him in YET

    v.haveRunner = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    v.haveRunner = true;
    v.runnerAtOrb = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::StageRunner);

    v.runnerAtOrb = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);

    // A runner that cannot legally click is replaced immediately — the 60s
    // lockout does not pause for the guard fight.
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // And a boss already in combat is just as good an answer, whatever pulled
    // him: a stray add, a wipe recovery, or the formation link a tick after the
    // tag. Either flag releases the wait.
    v.runnerCanClick = true;
    v.orbGuardsEngaged = false;
    v.bossEngaged = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, AGuardRespawnNeverAbortsALivePossession)
{
    // The guard gate is BEHIND the possessed branch, and behind completion. Once
    // the mind control is up the run has 90 seconds of egg to break and nothing
    // about the platform can be worth spending them on — the driver must keep
    // driving.
    DcRazorgore::View v = Driving();
    v.orbGuardsAlive = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::MoveBoss);

    v.bossToEgg = 1.0f;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::CastEgg);

    // ...and a guard standing after the last egg is still Done, not a new job.
    v.eggsRemaining = 0;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::Done);
}

TEST(DungeonEventBlackwingLairTest, GrethokIsBossZeroAndTheRealBossesShiftBehindHim)
{
    // The roster patch IS the fix for "the tank runs in and leaves the raid
    // behind": with no anchor of his own, Grethok was pulled by a bespoke rung
    // that glided every bot at the ledge; as boss #0 he is pulled by the ordinary
    // pipeline, which musters the raid and stages it first.
    std::vector<DungeonBossInfo> base;
    uint32 const entries[] = {12435, 13020, 12017, 11983, 14601, 11981, 14020, 11583};
    for (uint32 i = 0; i < 8; ++i)
    {
        DungeonBossInfo b;
        b.entry = entries[i];
        b.encounterIndex = i;   // instance_encounters 610-617, bits 0-7
        b.mapId = MAP_ID;
        base.push_back(b);
    }

    auto const out = BossRosterRegistry::Apply(MAP_ID, DcDiffKey::Raid(0), base);
    ASSERT_EQ(out.size(), 9u);

    // Grethok leads, and he is a BOSS anchor — an objective would complete on
    // arrival and never pull anything.
    ASSERT_EQ(out.front().entry, NPC_GRETHOK_THE_CONTROLLER);
    EXPECT_EQ(out.front().kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(BossOrderKey(out.front()), 0u);
    // His anchor is his spawn: the pull pipeline measures its standoff from the
    // live creature, but the advance walks the raid to this point to find him.
    EXPECT_FLOAT_EQ(out.front().x, GRETHOK_X);
    EXPECT_FLOAT_EQ(out.front().y, GRETHOK_Y);
    EXPECT_FLOAT_EQ(out.front().z, GRETHOK_Z);

    // COMPLETION is borrowed from Razorgore. Grethok has no DungeonEncounter row
    // of his own, so without this his row would carry bit 0 by default anyway —
    // the inheritance says so on purpose, and it must resolve to a REAL bit
    // rather than being left as a dangling "inherit from" marker.
    EXPECT_EQ(out.front().encounterIndex, 0u);
    EXPECT_EQ(out.front().inheritCompletionFrom, 0u)
        << "the inheritance must be resolved by Apply, not carried into the run";

    // ...and the eight real bosses keep their DBC kill-bits while shifting to
    // 1..8, so nothing about their completion detection changes.
    for (uint32 i = 0; i < 8; ++i)
    {
        EXPECT_EQ(out[i + 1].entry, entries[i]) << i;
        EXPECT_EQ(out[i + 1].encounterIndex, i) << i;
        EXPECT_EQ(BossOrderKey(out[i + 1]), i + 1) << i;
    }

    // Razorgore specifically must sort AFTER Grethok — a tie would leave the
    // order to the list's own sort and the raid would walk to the boss it is
    // forbidden to kill.
    EXPECT_LT(BossOrderKey(out.front()), BossOrderKey(out[1]));
}

TEST(DungeonEventBlackwingLairTest, TheCampRungSitsBetweenTheRaidStrategyAndTheRunner)
{
    // Two rungs left on this map, and the ordering between them and the raid
    // strategy is the whole statement of who owns a bot's tick during the egg
    // run. Above ACTION_RAID+1 (61): `bwl razorgore avoid aoe` would walk bots
    // out of the camp. Below the runner (62): a bot that is somehow both has the
    // more urgent job at the orb.
    EXPECT_GT(DcRel::RazorgoreCamp, 61.0f);
    EXPECT_LT(DcRel::RazorgoreCamp, DcRel::RazorgoreOrb);
}

TEST(DungeonEventBlackwingLairTest, ACastingRunnerIsNotDisqualified)
{
    // A DPS bot is casting most ticks. When "not mid-cast" was one of the
    // election's gates, the elected runner read unusable almost every tick, the
    // FSM asked for another, and the rotation cycled a fresh runner every three
    // seconds for the whole window without a single click landing. The cast is
    // interrupted at the orb instead, where it is one call.
    //
    // What runnerCanClick means now is the orb script's own three refusals, and
    // those still send the job to somebody else...
    DcRazorgore::View v = Driving();
    v.bossCharmed = false;
    v.runnerAtOrb = true;
    v.runnerCanClick = false;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::NeedRunner);

    // ...and a runner that merely has a spell in flight is not one of them: it is
    // still the runner, and it still clicks.
    v.runnerCanClick = true;
    EXPECT_EQ(DcRazorgore::Decide(v), DcRazorgore::Step::ClickOrb);
}

TEST(DungeonEventBlackwingLairTest, RazorgoreIsATargetExclusionRowOnHisOwnMap)
{
    // The guard lives in dungeon-clear, not in mod-playerbots' `bwl` strategy,
    // because GatherStrategyTargetExclusions walks EVERY strategy on the bot's
    // combat engine — DungeonClearCombatStrategy included. Keeping it here puts
    // the combat guard in the same module as the driver whose work it protects.
    EXPECT_TRUE(DcTargetExclusionRegistry::HasRowsFor(MAP_ID));

    // ...and nowhere else. HasRowsFor is the cheap gate mod-playerbots caches per
    // engine; a stray map here would make every bot on it rebuild its combat
    // strategy list on every target pick.
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(409));  // Molten Core
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(608));  // The Violet Hold
    EXPECT_FALSE(DcTargetExclusionRegistry::HasRowsFor(0));

    // The row is keyed to Razorgore specifically — the adds around him are
    // ordinary targets and must stay killable.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, 12422u));
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, MAP_ID, 12416u));
    // Right entry, wrong map: never excluded.
    EXPECT_FALSE(DcTargetExclusionRegistry::IsExcluded(nullptr, 409u, NPC_RAZORGORE));
}
