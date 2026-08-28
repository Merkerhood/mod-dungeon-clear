/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
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
