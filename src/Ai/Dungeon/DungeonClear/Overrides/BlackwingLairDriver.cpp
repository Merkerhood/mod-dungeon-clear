/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

// The Blackwing Lair driver — Razorgore's orb and egg run.
//
// ONE hook, and it is a controller in the Black Morass / Violet Hold sense: it
// re-decides from live world state every tick for the whole of phase 1. What
// makes it unlike either of those is WHAT it steers.
//
//   * IT NEVER MOVES THE BOT THAT RUNS IT. The Violet Hold driver's whole job is
//     walking the tank across the arena; this one's is walking a MIND-CONTROLLED
//     BOSS around a room while the bot running it — the raid's main tank — keeps
//     tanking. So every branch here ends in Done (yield the tick): the work is
//     side effects on other units, and claiming the tick would only starve the
//     tank's rotation for the ~135 seconds the egg run takes. Getting that
//     backwards wipes raids, which is the one lesson this module has paid for
//     twice already.
//   * IT DELEGATES ITS OTHER ACTOR. The orb refuses a user with a pet and roots
//     whoever takes it for 90 seconds, so the runner cannot be the tank. The
//     driver ELECTS one and publishes it (DcRunState::razorRunnerGuid, read
//     cross-bot through DcLeaderSignal::GetRazorgoreOrbStation); the runner
//     walks itself to the orb and clicks it on its OWN tick, in
//     DungeonClearRazorgoreOrbAction. Reaching across and poking another bot's
//     MotionMaster from here would fight both that bot's AI and the `bwl`
//     strategy's own repositioning, every tick, for ever.
//
// THE ENCOUNTER'S CLOCKS, all measured, none of them ours to choose:
//   mind control 90s (19832) · charmer lockout 60s (23958) · egg cast 3s and
//   10yd (19873, interrupt-on-move) · 30 eggs · a ~262yd tour of them. Two to
//   three windows, and a rotation of runners because the lockout outlives a
//   single window by design.
//
// WHAT WE DO NOT DO HERE: keep the raid off the boss. Killing Razorgore in phase
// 1 instakills everyone (20038), and that guard is real and DC's own, but it lives
// in the target-selection seam rather than in this driver —
// DcTargetExclusionRegistry, DungeonClearDpsTargetValue and the hold-fire rung,
// which between them bar him from every DPS pick and take him back off anyone who
// already had him. The best thing this driver can do about the risk is what it
// already does: finish phase 1 quickly.
//
// AND WHAT WE NO LONGER DO: walk the raid to the orb platform. Grethok the
// Controller is a boss anchor now (RegisterBlackwingLairRoster), so the ordinary
// pipeline — advance, raid muster, boss standoff, engage — brings the raid up as
// one body and the tank makes the pull. This driver waits for that pull and then
// works the orb; it publishes ONE stamp (razorDrivingMs) which arms the raid's
// egg-run camp from the pull onward, and nothing before it.

namespace
{
    using namespace DcBlackwingLair;

    // Movement point id for the boss's egg-to-egg splines. Arbitrary but stable:
    // it only has to differ from anything the boss's own AI would use, and its
    // AI is not running any movement while charmed (SetCharmedBy idles the
    // MotionMaster and UpdateVictim fails, so UpdateAI early-outs).
    constexpr uint32 EGG_MOVE_POINT_ID = 4690;

    // Don't re-issue the boss's spline more than this often. A MovePoint every
    // tick re-plots the path and the unit travels nowhere; ~1s is well inside the
    // 1-2s a leg between neighbouring eggs takes.
    constexpr uint32 MOVE_REISSUE_MS = 1000;

    // Re-election throttle for the runner, so a comp with nobody eligible logs
    // once a few seconds instead of once a tick.
    constexpr uint32 RUNNER_PICK_MS = 3000;

    // --- world lookups -----------------------------------------------------

    Creature* BwlRazorgore(Player* bot)
    {
        return bot ? bot->FindNearestCreature(NPC_RAZORGORE, ROOM_SCAN, /*alive*/ true) : nullptr;
    }

    GameObject* BwlOrb(Player* bot)
    {
        return bot ? bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, ROOM_SCAN) : nullptr;
    }

    // Every egg still standing. A destroyed egg does not linger in a flagged
    // state to be filtered later: spell_egg_event Uses the goober and pushes its
    // respawn a week out, so it flips to ACTIVE and despawns within ten seconds.
    // Scanning for `isSpawned() && GO_STATE_READY` is therefore an exact count of
    // the work left, and it survives a wipe, a re-entry and the phase flip (which
    // phases the leftovers out) without any bookkeeping of our own.
    void BwlLiveEggs(Creature* razor, std::vector<GameObject*>& out)
    {
        out.clear();
        if (!razor)
            return;
        std::list<GameObject*> found;
        razor->GetGameObjectListWithEntryInGrid(found, GO_BLACK_DRAGON_EGG, ROOM_SCAN);
        for (GameObject* go : found)
            if (go && go->isSpawned() && go->GetGoState() == GO_STATE_READY)
                out.push_back(go);
    }

    bool BwlIsSkipped(DcRunState const& st, ObjectGuid guid)
    {
        for (ObjectGuid const& g : st.razorEggSkipped)
            if (g == guid)
                return true;
        return false;
    }

    // --- the runner --------------------------------------------------------

    // Is this member still a legal orb user RIGHT NOW? The orb script's own three
    // refusals (dead, exhausted, owns a pet) plus "is a bot we can drive".
    // Re-read every tick rather than remembered: a hunter that resummoned its pet
    // and a runner that just took the 60s lockout look identical to a cached flag.
    bool BwlRunnerUsable(Player* p)
    {
        return p && p->IsAlive() && GET_PLAYERBOT_AI(p) && !p->GetPet() &&
               !p->HasAura(SPELL_MIND_EXHAUSTION);
    }

    Player* BwlResolveRunner(Player* leader, DcRunState const& st)
    {
        if (!leader || !st.razorRunnerGuid)
            return nullptr;
        Player* p = ObjectAccessor::FindPlayer(st.razorRunnerGuid);
        if (!p || p->GetMapId() != leader->GetMapId())
            return nullptr;
        return p;
    }

    // Build the candidate pool and hand it to the pure elector. The pool is the
    // whole raid on this map, not a sub-group: the orb is one object and the
    // rotation has to draw from everybody who can legally take it.
    // `force` skips the throttle. The throttle exists so a comp with nobody
    // eligible logs once every few seconds instead of once a tick; it must NOT
    // stand between a runner whose possession just broke and its replacement,
    // because those three seconds are three seconds of a freed Razorgore beating
    // on a DPS the raid is forbidden to help by killing him.
    Player* BwlElectRunner(Player* leader, DcRunState& st, bool force = false)
    {
        uint32 const now = getMSTime();
        if (!force && st.razorRunnerPickedMs && now - st.razorRunnerPickedMs < RUNNER_PICK_MS)
            return BwlResolveRunner(leader, st);
        st.razorRunnerPickedMs = now;

        Group* group = leader ? leader->GetGroup() : nullptr;
        if (!group)
            return nullptr;

        std::vector<DcRazorgore::RunnerCandidate> pool;
        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* p = ref->GetSource();
            if (!p || p->GetMapId() != leader->GetMapId())
                continue;

            DcRazorgore::RunnerCandidate c;
            c.guidLow   = p->GetGUID().GetCounter();
            c.alive     = p->IsAlive();
            c.isBot     = GET_PLAYERBOT_AI(p) != nullptr;
            c.isLeader  = (p == leader);
            c.isTank    = PlayerbotAI::IsTank(p);
            c.isHealer  = PlayerbotAI::IsHeal(p);
            c.isRanged  = PlayerbotAI::IsRanged(p);
            c.hasPet    = p->GetPet() != nullptr;
            c.exhausted = p->HasAura(SPELL_MIND_EXHAUSTION);
            // Straight-line to the orb, not a path length: the tie-break is
            // bucketed at 10yd and everyone in the pool is in the same chamber, so
            // a pathfinder call per candidate per election would buy nothing.
            c.distToOrb = p->GetExactDist2d(ORB_X, ORB_Y);
            pool.push_back(c);
            members.push_back(p);
        }

        int const pick = DcRazorgore::SelectRunner(pool);
        if (pick < 0)
        {
            // Nobody eligible. Usually transient — every candidate is inside the
            // 60s lockout between windows — but a comp of pet classes makes it
            // permanent, and that is worth saying out loud rather than stalling
            // in silence.
            st.razorRunnerGuid.Clear();
            LOG_WARN("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — no eligible orb runner among {} members "
                     "(needs a live bot with no pet and no Mind Exhaustion)",
                     uint32(members.size()));
            return nullptr;
        }

        Player* runner = members[pick];
        if (st.razorRunnerGuid != runner->GetGUID())
        {
            st.razorRunnerGuid = runner->GetGUID();
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — orb runner is {}", runner->GetName());
        }
        return runner;
    }

    // --- steering the possessed boss ---------------------------------------

    // Nearest live, non-skipped egg to the boss. Greedy nearest-neighbour: the
    // measured tour of all thirty from his spawn is 262yd, which at his 10 yd/s
    // is 26 seconds against 90 seconds of casting — the order is simply not where
    // this encounter's time goes, so the cheapest correct rule wins.
    GameObject* BwlElectEgg(Creature* razor, std::vector<GameObject*> const& eggs,
                            DcRunState const& st)
    {
        GameObject* best = nullptr;
        float bestDist = 0.0f;
        for (GameObject* egg : eggs)
        {
            if (BwlIsSkipped(st, egg->GetGUID()))
                continue;
            float const d = razor->GetExactDist(egg);
            if (!best || d < bestDist)
            {
                best = egg;
                bestDist = d;
            }
        }
        return best;
    }

    void BwlStopBoss(Creature* razor)
    {
        if (!razor)
            return;
        MotionMaster* mm = razor->GetMotionMaster();
        if (mm && mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            // Clear(false) + MoveIdle is the same incantation SetCharmedBy uses to
            // park a freshly possessed creature, and it is what actually removes
            // the generator: StopMoving alone leaves it installed and it re-asserts
            // on the next motion update — the persistent-generator gotcha that
            // walks a "stopped" unit right back out.
            mm->Clear(false);
            mm->MoveIdle();
        }
        razor->StopMoving();
    }

    void BwlMoveBossTo(Creature* razor, GameObject* egg, DcRunState& st)
    {
        uint32 const now = getMSTime();
        bool const sameTarget = st.razorEggGuid == egg->GetGUID();
        bool const travelling = razor->GetMotionMaster() &&
                                razor->GetMotionMaster()->GetCurrentMovementGeneratorType() ==
                                    POINT_MOTION_TYPE;
        if (sameTarget && travelling && now - st.razorMoveIssuedMs < MOVE_REISSUE_MS)
            return;

        st.razorMoveIssuedMs = now;
        // generatePath, and NOT forceDestination. The room has two tiers joined by
        // a ramp, so a straight-line override would happily slide the boss through
        // the wall of the ledge; a generated path that ends short is a stall we can
        // SEE (the no-progress clock below retires the egg) and a wall-clip is not.
        razor->GetMotionMaster()->MovePoint(EGG_MOVE_POINT_ID, egg->GetPositionX(),
                                            egg->GetPositionY(), egg->GetPositionZ(),
                                            FORCED_MOVEMENT_NONE, /*speed*/ 0.0f,
                                            /*orientation*/ 0.0f, /*generatePath*/ true,
                                            /*forceDestination*/ false);
    }

    // Destroy one egg. The polite cast first — a real 3s channel, exactly what a
    // player's possess bar does — and after enough tries on the SAME egg, the same
    // spell TRIGGERED. That is not a different outcome: 19873 carries the whole
    // encounter's bookkeeping in spell_egg_event::HandleOnHit (the instance
    // counter, the boss's line, the goober use, the week-long respawn), and a
    // triggered cast runs the identical script with the identical caster and
    // target. All it skips is the channel.
    //
    // The counter counts ATTEMPTS, not refusals, and that distinction is the whole
    // point of it. 19873's interrupt flags are 31 — movement, pushback, school
    // lock, autoattack and DAMAGE all cancel it — so the failure this has to
    // survive is not a refused cast (which returns a result we can read) but a
    // STARTED one that never finishes: CastSpell says OK, three seconds later
    // something has hit the boss, and the egg is still standing with nothing in
    // the return value to say so. Counting starts catches both shapes; the caller
    // resets the counter when the elected egg changes, so it only ever measures
    // failures against ONE egg.
    void BwlCastEggDestroy(Creature* razor, GameObject* egg, DcRunState& st)
    {
        razor->SetFacingToObject(egg);

        bool const triggered = st.razorEggAttempts >= DcRazorgore::kPoliteCastAttempts;
        SpellCastResult const res = razor->CastSpell(egg, SPELL_DESTROY_EGG, triggered);
        ++st.razorEggAttempts;

        if (res != SPELL_CAST_OK || triggered)
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — Destroy Egg on {} -> result {}, attempt {}{}",
                      egg->GetGUID().ToString(), uint32(res), uint32(st.razorEggAttempts),
                      triggered ? " [triggered]" : "");
    }
}

// The hook. Ordered by the pure kernel (DcRazorgore::Decide); everything below is
// the glue that turns one Step into world side effects.
//
// ALWAYS returns Done. Done means "nothing to steer this tick" and yields, and
// that is right on every branch here because the driver never moves the bot it
// runs on — the tank has an add wave to hold and needs its rotation. The event is
// Repeatable, so a Done simply re-arms it for the next tick.
static ObjectiveArriveResult DriveRazorgoreOrb(Player* bot, AiObjectContext* context,
                                               DungeonBossInfo const& /*info*/)
{
    if (!bot || !context || bot->GetMapId() != DcBlackwingLair::MAP_ID)
        return ObjectiveArriveResult::Done;

    using namespace DcBlackwingLair;
    DcRunState& st = DcRun::Of(context);

    Creature* razor = BwlRazorgore(bot);
    std::vector<GameObject*> eggs;
    BwlLiveEggs(razor, eggs);

    InstanceScript* inst = bot->GetInstanceScript();

    DcRazorgore::View v;
    v.eventDone      = inst && inst->GetData(DATA_EGG_EVENT) == DONE;
    v.bossAlive      = razor != nullptr;
    v.eggsRemaining  = static_cast<uint32>(eggs.size());

    // ADOPT WHOEVER ACTUALLY HOLDS HIM. The driver's election is a preference;
    // the charm is a fact. They come apart in the one case that matters — a
    // possession that broke early, an election that has since moved on, and a
    // second bot about to click an orb on a boss somebody else is already
    // driving — and every time they do, the world is right and the election is
    // wrong. So the charmer is re-published as the runner before anything is
    // decided, and the two can never disagree for more than a tick.
    if (razor && razor->IsCharmed())
    {
        ObjectGuid const charmer = razor->GetCharmerGUID();
        if (charmer.IsPlayer() && charmer != st.razorRunnerGuid)
        {
            Player* holder = ObjectAccessor::FindPlayer(charmer);
            if (holder && GET_PLAYERBOT_AI(holder) && holder->GetMapId() == bot->GetMapId())
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — adopting {} as the orb runner "
                         "(it holds the possession)", holder->GetName());
                st.razorRunnerGuid = holder->GetGUID();
            }
        }
    }

    Player* runner = BwlResolveRunner(bot, st);
    v.bossCharmed = razor && runner && razor->IsCharmed() &&
                    razor->GetCharmerGUID() == runner->GetGUID();
    v.bossCasting = razor && razor->HasUnitState(UNIT_STATE_CASTING);
    // Pulling the guard pack pulls the boss: creature_formations makes Razorgore
    // a member of Grethok's formation with groupAI 7 (full mutual assist), so the
    // tank's tag on Grethok aggros him from 77yd on the next tick. Either flag is
    // "the pull has landed" as far as the kernel is concerned.
    v.bossEngaged = razor && razor->IsInCombat();

    // The platform. Only asked while the boss is NOT charmed — mid-possession the
    // guards are long dead and the scan is pure cost, and a stray respawn must
    // never abort a live egg run.
    //
    // Out of the chamber the answer is "held and unpulled", not "clear". The event
    // is due from 200yd out (the leader may still be on the approach) and the SAFE
    // reading of an unscanned platform is the one that moves nobody: everything
    // the kernel does with a live body starts on the far side of that answer.
    bool const nearChamber = bot->GetExactDist2d(ORB_X, ORB_Y) <= GUARD_CLEAR_RANGE;
    OrbGuardState const guards =
        (!v.bossCharmed && nearChamber) ? DcBlackwingLair::OrbGuards(bot) : OrbGuardState{};
    v.orbGuardsAlive   = !v.bossCharmed && (!nearChamber || guards.alive);
    v.orbGuardsEngaged = guards.engaged;

    // THE ANCHOR LATCH, and the one thing here that talks to the clear rather than
    // to the encounter. Grethok's roster row borrows Razorgore's kill-bit, which
    // covers everything up to the moment the raid actually kills the boss — but a
    // phase-1 wipe leaves a Grethok who is dead, decayed, and will never respawn,
    // and a candidate row for a creature that is not on the map stalls the clear
    // ("not spawned. Use 'dc skip'"). An empty platform IS his completion, so say
    // so once and let the roster advance to Razorgore on its own.
    //
    // Only on a tick that actually SCANNED the platform: mid-possession the scan
    // is skipped (it is pure cost with a charm up) and `guards` reads its default
    // all-false, which is not the same fact.
    if (nearChamber && !v.bossCharmed && !guards.alive && !v.eventDone)
    {
        auto& cleared =
            context->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get();
        if (cleared.insert(NPC_GRETHOK_THE_CONTROLLER).second)
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — the orb platform is clear; "
                     "Grethok's anchor is done");
    }

    v.haveRunner     = runner && runner->IsAlive() && GET_PLAYERBOT_AI(runner);
    v.runnerAtOrb    = runner && runner->GetExactDist2d(ORB_X, ORB_Y) <= ORB_STATION_RADIUS;
    // The orb script's own refusals ONLY. "Mid-cast" used to be in here and was a
    // bug with a signature: a DPS bot is casting most ticks, so the elected runner
    // read unusable, the FSM asked for another, and the rotation cycled a new
    // runner every three seconds for the whole window without a single click
    // landing. A cast in flight is interrupted at the orb instead — see
    // DungeonClearRazorgoreOrbAction.
    v.runnerCanClick = BwlRunnerUsable(runner);

    GameObject* egg = nullptr;
    if (v.bossCharmed)
    {
        egg = BwlElectEgg(razor, eggs, st);
        v.haveEgg = egg != nullptr;
        if (egg)
            v.bossToEgg = razor->GetExactDist(egg);
    }

    // One line per 3s carrying every number the hook decided on. A run that goes
    // wrong is diagnosed from this: eggs standing still with a charm up means the
    // casts are not landing; "NeedRunner" forever means the comp has nobody legal;
    // a charm that never appears means the click is being swallowed.
    DcRazorgore::Step const step = DcRazorgore::Decide(v);
    {
        thread_local std::unordered_map<uint32, uint32> lastMs;
        uint32& prev = lastMs[bot->GetGUID().GetCounter()];
        if (!prev || GetMSTimeDiffToNow(prev) > 3000)
        {
            prev = getMSTime();
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — step {}, eggs {}/{}, guards {}, charmed {}, "
                      "runner {} (atOrb {}, canClick {}), egg {:.1f}yd, skipped {}",
                      uint32(step), v.eggsRemaining, EGG_COUNT,
                      v.orbGuardsAlive
                          ? ((v.orbGuardsEngaged || v.bossEngaged) ? "true/pulled" : "true/unpulled")
                          : "false",
                      v.bossCharmed,
                      runner ? runner->GetName() : "none", v.runnerAtOrb, v.runnerCanClick,
                      v.haveEgg ? v.bossToEgg : -1.0f, uint32(st.razorEggSkipped.size()));
        }
    }

    // Publish the ONE stamp the raid's positioning reads cross-bot: "the egg run
    // owns this fight". It arms the camp rung (the raid holds the floor below the
    // ledge, between the adds and the rooted runner) and keeps the elected
    // runner's own rung alive.
    //
    // Stamped from the PULL onward — never before it. Up to the tag, positioning
    // belongs to the clear: the advance is walking the raid to Grethok's anchor
    // and the muster is topping it off, and a camp rung armed underneath that
    // would fight the pipeline for every bot. WaitPull is exactly "the tank has
    // not pulled yet", so it is the one step that publishes nothing; Done is the
    // other, so the camp releases the instant phase 1 ends without a completion
    // test of its own.
    if (step != DcRazorgore::Step::Done && step != DcRazorgore::Step::WaitPull)
        st.razorDrivingMs = getMSTime();

    switch (step)
    {
        case DcRazorgore::Step::Done:
            // Phase 1 is over (or the boss is gone). Drop every scrap of state so a
            // re-entry, a wipe, or the SECOND time this instance is run starts
            // clean — the encounter soft-resets itself on a phase-1 death and we
            // must not come back holding a stale runner or a stale skip list.
            st.ClearRazorgore();
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::Idle:
            // Every remaining egg is on the skip list. Clear it and take another
            // pass: a skip is a "not now" (a partial path, a cast that would not
            // land), never a "never", and the eggs that stalled early are often
            // reachable from where the boss has since ended up.
            if (!st.razorEggSkipped.empty())
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — retrying {} skipped egg(s)",
                         uint32(st.razorEggSkipped.size()));
                st.razorEggSkipped.clear();
                st.razorEggGuid.Clear();
            }
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::WaitPull:
            // The platform is held and nobody has pulled it. This is the tank's
            // tick, not ours: Grethok is boss anchor #0 and the ordinary pipeline
            // is walking the raid to him, mustering it and taking the standoff.
            // Deliberately NOTHING here — not even an election, whose only effect
            // before the pull would be to arm a runner's rung and send one DPS bot
            // up the ramp ahead of its raid.
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::NeedRunner:
            // FORCE the election whenever a runner exists but can no longer take
            // the orb. That is the broken-possession path — its Mind Exhaustion is
            // up for the next sixty seconds and Razorgore is on it — and the
            // three-second throttle is a debounce for an empty candidate pool, not
            // a reason to leave the raid without a runner while that plays out.
            BwlElectRunner(bot, st, /*force*/ v.haveRunner && !v.runnerCanClick);
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::StageRunner:
            // The runner walks itself — see DungeonClearRazorgoreOrbAction, which
            // reads the published GUID through DcLeaderSignal. Nothing to do here
            // but keep the election fresh in case it died on the way.
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::ClickOrb:
        {
            GameObject* orb = BwlOrb(bot);
            if (!orb)
                return ObjectiveArriveResult::Done;
            // GameObject::Use dispatches to go_orb_of_domination::GossipHello
            // before it ever looks at the goober type, so this IS the player's
            // click: the script sets the boss's charmer, has it attack the runner,
            // and casts 19832 from the runner. We do not fake any of that.
            //
            // The runner does the clicking on its own tick (it is the one that
            // knows it has arrived and is not mid-cast); this branch exists for the
            // case where the runner's own rung is not the thing that got here —
            // it is idempotent, because a second click while already charmed is
            // refused by the script's own DONE / exhaustion / pet gates.
            LOG_INFO("playerbots.dungeonclear",
                     "DungeonClear: Razorgore — {} takes the Orb of Domination",
                     runner->GetName());
            orb->Use(runner);
            return ObjectiveArriveResult::Done;
        }

        case DcRazorgore::Step::WaitCast:
            return ObjectiveArriveResult::Done;

        case DcRazorgore::Step::MoveBoss:
        {
            // No-progress clock on the ELECTED EGG, not on the run: the failure this
            // catches is one egg the boss cannot reach (a path that ends short of
            // the ledge), and the answer is to spend the window on the other
            // twenty-nine rather than to stall the encounter on this one.
            uint32 const now = getMSTime();
            if (st.razorEggGuid != egg->GetGUID())
            {
                st.razorEggGuid     = egg->GetGUID();
                st.razorEggElectedMs = now;
                st.razorEggBestDist = v.bossToEgg;
                st.razorEggAttempts = 0;
            }
            else if (v.bossToEgg < st.razorEggBestDist - DcRazorgore::kRepathEpsilon)
            {
                st.razorEggBestDist = v.bossToEgg;
                st.razorEggElectedMs = now;
            }
            else if (now - st.razorEggElectedMs > DcRazorgore::kEggStallMs)
            {
                LOG_INFO("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — egg {} unreachable ({:.1f}yd, no progress "
                         "in {}ms) -> skipping for this pass",
                         egg->GetGUID().ToString(), v.bossToEgg, DcRazorgore::kEggStallMs);
                st.razorEggSkipped.push_back(egg->GetGUID());
                st.razorEggGuid.Clear();
                return ObjectiveArriveResult::Done;
            }

            BwlMoveBossTo(razor, egg, st);
            return ObjectiveArriveResult::Done;
        }

        case DcRazorgore::Step::CastEgg:
        {
            // Stop FIRST. 19873 carries interrupt flags 31 — any movement during
            // the 3s cast eats it — and a unit that is still sliding out of its
            // last spline is moving even when it looks parked.
            BwlStopBoss(razor);

            uint32 const now = getMSTime();
            if (st.razorEggGuid != egg->GetGUID())
            {
                st.razorEggGuid      = egg->GetGUID();
                st.razorEggElectedMs = now;
                st.razorEggBestDist  = v.bossToEgg;
                st.razorEggAttempts  = 0;
            }

            BwlCastEggDestroy(razor, egg, st);

            // In range, stationary, and still standing after both the polite and
            // the triggered attempts have had their fifteen seconds. Park it and
            // spend the window on the other twenty-nine.
            if (now - st.razorEggElectedMs > DcRazorgore::kEggStallMs)
            {
                LOG_WARN("playerbots.dungeonclear",
                         "DungeonClear: Razorgore — egg {} survived {} cast attempt(s) "
                         "-> skipping for this pass",
                         egg->GetGUID().ToString(), uint32(st.razorEggAttempts));
                st.razorEggSkipped.push_back(egg->GetGUID());
                st.razorEggGuid.Clear();
            }
            return ObjectiveArriveResult::Done;
        }
    }

    return ObjectiveArriveResult::Done;
}

// Id 20. The Violet Hold's are 15-19; ids are one flat space across every
// dungeon, and AddHook LOG_ERRORs a collision rather than silently dropping one.
void RegisterBlackwingLairHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, DcBlackwingLair::HOOK_RAZORGORE_ORB,
                                   &DriveRazorgoreOrb);
}
