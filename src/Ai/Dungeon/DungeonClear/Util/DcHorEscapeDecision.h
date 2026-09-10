/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCHORESCAPEDECISION_H
#define _PLAYERBOT_DCHORESCAPEDECISION_H

#include <cstdint>

#include "Define.h"

// PURE kernel for the Halls of Reflection (map 668) escape — the whole of the
// escape driver's per-tick reasoning, without a map, a creature or an instance.
//
// The glue (grid scans, the wall-GO read, the splines, the telemetry) lives in
// Overrides/HallsOfReflectionDriver.cpp.
//
// THE ENCOUNTER, in the terms these functions use. From the moment the leader's
// gossip is accepted, the Lich King (36954) walks a fixed 19-waypoint path at
// 1.445 yd/s under Remorseless Winter and NEVER PAUSES. Jaina/Sylvanas runs ahead
// of him and stops at four of those waypoints, each ~24yd short of an Ice Wall
// she cannot pass; each wall opens only when the WHOLE summon batch the Lich King
// spat out behind the party is dead. He reaches 12.5yd of her at T0+75.6 / +156 /
// +233 / +355, and when he does he catches her, kills her three seconds later and
// wipes the party with Fury of Frostmourne.
//
// SO THE CLOCK IS ABSOLUTE AND THE THROUGHPUT IS NOT THE PROBLEM. A level-80
// party clears each batch with margin; what loses this fight is POSITION. Three
// rules are enforced by the encounter itself and all three are geometric:
//
//   1. THE CATCH IS ON THE LEADER, NOT ON PLAYERS. Nothing a bot does can be
//      "caught". The only way players lose her is by not clearing a wall in time.
//
//   2. FALLING BEHIND HIM COSTS 10 000 DAMAGE AND A KNOCKBACK. Every 2 seconds
//      while Winter is up, each alive player with (p.x - lk.x) + (p.y - lk.y) >
//      20 is Zapped (70653) — and because the whole path runs -x AND -y, that
//      single scalar IS "behind him". The knockback throws the victim further
//      behind, so the rule is self-reinforcing and has to be pre-empted rather
//      than reacted to. LK_BEHIND_SUM is a third of the way to the line.
//
//   3. INSIDE 10YD OF HIM IS 7068 +/- 863 FROST PER SECOND. LK_PRESSURE_DIST is
//      that plus margin.
//
// Both corrections are the SAME MOVE — forward along the path, to the party's
// stand point — and that is the single most important property of this kernel. A
// radial "get away from the emitter" vacate is exactly wrong for a bot that is
// already behind him: away-from-him is further behind, back into the zap rule.
// See DungeonClearHorStayAheadAction for the per-follower half of the same rule.
//
// WHAT THE KERNEL DOES *NOT* MODEL, deliberately:
//
//   * currentWall. npc_hor_lich_kingAI keeps it privately and nothing exposes it.
//     The observable equivalent is THE LEADER'S OWN POSITION — she runs to the
//     next stop the instant WallCompleted fires — so `leaderStop` is the state
//     variable, and `wallOpen` (read off the Ice Wall gameobject) is a second,
//     faster witness of the same transition. Either advances the party.
//
//   * how much health the adds have left. ADVANCE fires on the WALL, never on
//     "the adds are dead", because on heroic the last abomination of wall 4 can
//     still be alive when the wall opens and waiting for it would spend the
//     margin that wall's 1.17M HP has already eaten.
namespace DcHorEscape
{
    enum class State : uint8
    {
        Done = 0,    // the escape is not running (not started, finished, or reset)
        Prelude,     // gossip accepted, walking to the first stand point
        Hold,        // standing at the stand point with nothing to do
        Fight,       // the batch is on the party — yield to the rotation
        Pressure,    // too close to him, or behind him — step forward NOW
        Advance,     // the wall opened / the leader moved on — take the next stand
        Final,       // wall 4 is down; run the last 131yd to WP18 with her
        Doomed,      // Harvest Soul is on the leader; Fury lands in 3 seconds
    };

    inline char const* StateName(State s)
    {
        switch (s)
        {
            case State::Prelude:  return "prelude — getting to the first wall ahead of him";
            case State::Hold:     return "holding the stand point";
            case State::Fight:    return "fighting the wall's summons";
            case State::Pressure: return "TOO CLOSE / BEHIND HIM — stepping forward";
            case State::Advance:  return "the wall is open — moving up";
            case State::Final:    return "wall 4 is down — running for the gunship";
            case State::Doomed:   return "the leader has been caught; Fury of Frostmourne is coming";
            case State::Done:
            default:              return "not running";
        }
    }

    struct Inputs
    {
        // --- is this ours ----------------------------------------------------

        // GetBossState(DATA_LICH_KING) == IN_PROGRESS. Raised by exactly one
        // thing (the escape gossip in hook 34) and cleared by DONE or by the
        // full-reset FAIL, so it is both the predicate and the near-gate.
        bool active = false;

        // --- the leader -------------------------------------------------------
        bool leaderAlive = false;

        // Harvest Soul (70070) is on her: she dies in three seconds and the party
        // dies with her. Nothing useful is left to decide.
        bool leaderHasHarvestSoul = false;

        // WHICH WP_STOP she is at or heading for: the nearest stop within
        // LEADER_STOP_SNAP, else the next one along the path. 0 is the start
        // (WP0), 1..4 the four walls, 5 the end (WP18).
        uint8 leaderStop = 0;

        // --- the wall ---------------------------------------------------------

        // The Ice Wall gameobject at the CURRENT wall is absent or GO_STATE_ACTIVE
        // — i.e. it has opened. A faster witness of WallCompleted than the leader
        // actually getting moving, and the one heroic wall 4 needs (see the header
        // note): the party leaves on the wall, not on the last abomination.
        bool wallOpen = false;

        // --- the Lich King ----------------------------------------------------

        // Remorseless Winter (69780) is on him. While it is NOT, neither the zap
        // rule nor the ring exists — that is true both for the ~16s before he
        // casts it and for ever after the fourth wall falls.
        bool lkHasWinter = false;

        // THIS BOT's distance to him, and its own (x + y) minus his. The second is
        // the core's own scalar, unscaled: > 20 is a Zap, and LK_BEHIND_SUM is the
        // fraction of it the driver acts at.
        float distToLk = 0.0f;
        float sumMinusLkSum = 0.0f;

        // How near he has got to the LEADER, for the stall watchdog only.
        float lkToLeaderDist = 0.0f;

        // --- geometry and the fight --------------------------------------------
        float distToStand = 0.0f;   // this bot -> the stand point for the target stop
        float standLeash = 0.0f;
        uint32 addsAlive = 0;
        bool   partyInCombat = false;

        // --- the two thresholds -------------------------------------------------
        float pressureDist = 0.0f;  // DcHallsOfReflection::LK_PRESSURE_DIST
        float behindSum = 0.0f;     // DcHallsOfReflection::LK_BEHIND_SUM

        // --- clocks --------------------------------------------------------------
        uint32 nowMs = 0;
        uint8  state = 0;
        uint32 stateSinceMs = 0;
        uint8  stallStop = 0;       // the stop the stall report was last fired for
        bool   stallReported = false;
        float  stallDist = 0.0f;    // DcHallsOfReflection::LK_STALL_DIST
        uint32 stallMs = 0;
        uint32 stopHoldSinceMs = 0; // when the party first stood at THIS stop
    };

    struct Verdict
    {
        State state = State::Done;

        // WHICH stand point the party should be on: 1..4 are the four walls, 5 is
        // WP18. Always at least 1 — the party is ahead of the leader, never on the
        // start waypoint she is leaving.
        uint8 targetStop = 1;

        // --- what the driver should DO ---------------------------------------
        bool travel = false;     // TravelTo the stand point for targetStop
        bool holdStill = false;  // issue no movement, but claim the tick
        bool yieldTick = false;  // hand the tick to the stock combat engine

        // --- derived facts the glue logs or stores ---------------------------
        bool complete = false;
        bool reportStall = false;
        bool reportDoomed = false;

        uint8  storeState = 0;
        uint32 stateSinceMs = 0;
        uint8  stallStop = 0;
        bool   stallReported = false;
        uint32 stopHoldSinceMs = 0;
    };

    // Which stand point the party should hold, given where the leader is. The
    // party is always AHEAD of her: at the start (stop 0) it is already walking to
    // stand 1, and once she is at stop k it stands a few yards past stop k.
    //
    // Clamped at the top because stop 5 is WP18 — the end of the path, where there
    // is no wall and no offset and the party simply stands with her.
    inline uint8 TargetStopFor(uint8 leaderStop)
    {
        if (leaderStop < 1)
            return 1;
        return leaderStop > 5 ? 5 : leaderStop;
    }

    inline Verdict Decide(Inputs const& in)
    {
        Verdict v;
        v.stateSinceMs = in.stateSinceMs;
        v.stallStop = in.stallStop;
        v.stallReported = in.stallReported;
        v.stopHoldSinceMs = in.stopHoldSinceMs;
        v.targetStop = TargetStopFor(in.leaderStop);

        // --- 1. is the escape ours? ------------------------------------------
        //
        // Also treated as "not ours" when the leader is gone: the escape cannot
        // be completed without her (the walls open only for her channel and the
        // boss state only flips when the Lich King reaches WP17 with four walls
        // down), so a dead leader is a run wipe and the honest thing is to stop
        // claiming ticks the party could spend surviving.
        if (!in.active || !in.leaderAlive)
        {
            v.state = State::Done;
            v.complete = true;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Done);
            v.stateSinceMs = 0;
            v.stallStop = 0;
            v.stallReported = false;
            v.stopHoldSinceMs = 0;
            return v;
        }

        // --- 2. the two rules that outrank everything ------------------------
        //
        // DOOMED FIRST, because it is not a state the driver can act in. Harvest
        // Soul is on her, summonsCount has been set to 255 so no wall can ever
        // open again, and Fury of Frostmourne lands in three seconds. Say so once
        // and yield; there is no positioning that survives it.
        if (in.leaderHasHarvestSoul)
        {
            v.state = State::Doomed;
            v.yieldTick = true;
            v.storeState = static_cast<uint8>(State::Doomed);
            if (in.state != v.storeState)
            {
                v.stateSinceMs = in.nowMs;
                v.reportDoomed = true;
            }
            return v;
        }

        // PRESSURE SECOND, and it OVERRIDES FIGHT. Both of its triggers cost more
        // per second than any add does: 7068 frost/s inside the ring, or 10 000
        // plus a knockback that makes the next check worse. The move is always
        // FORWARD to the stand point — never radially away from him, which for a
        // bot that is already behind is a step deeper into the zap rule.
        //
        // Gated on Winter, because that is what gates both of the encounter's own
        // rules: before he casts it and after the fourth wall removes it, standing
        // near him is merely pointless rather than fatal.
        bool const pressured = in.lkHasWinter &&
                               (in.distToLk < in.pressureDist ||
                                in.sumMinusLkSum > in.behindSum);

        // --- 3. which state ---------------------------------------------------
        if (pressured)
            v.state = State::Pressure;
        else if (v.targetStop >= 5)
            v.state = State::Final;
        else if (in.distToStand > in.standLeash)
            v.state = in.leaderStop < 1 ? State::Prelude : State::Advance;
        else if (in.addsAlive > 0 || in.partyInCombat)
            v.state = State::Fight;
        else
            v.state = State::Hold;

        v.storeState = static_cast<uint8>(v.state);
        if (in.state != v.storeState)
            v.stateSinceMs = in.nowMs;

        // --- 4. the stall watchdog --------------------------------------------
        //
        // ONE LINE PER STOP, and only for the shape that is genuinely
        // unrecoverable from inside the driver: he is closing on her, the wall is
        // still shut, and the adds that hold it are not dying. On this map the
        // known cause is a Risen Witch Doctor parked inside the 10yd ring — it
        // casts from 20yd at the rearmost player, standing on HIS side, so melee
        // cannot finish it without eating the pulse and the pressure rule keeps
        // pulling them off it. Naming which adds are alive and where they stand is
        // what turns that from "the run stalled at wall 3" into a fix.
        //
        // Re-armed per stop rather than per state, because the party legitimately
        // cycles Hold -> Fight -> Hold several times at one wall.
        if (v.stallStop != v.targetStop)
        {
            v.stallStop = v.targetStop;
            v.stallReported = false;
            v.stopHoldSinceMs = 0;
        }

        bool const wallShut = !in.wallOpen && in.addsAlive > 0;
        if (wallShut && in.lkToLeaderDist > 0.0f && in.lkToLeaderDist <= in.stallDist)
        {
            if (!v.stopHoldSinceMs)
                v.stopHoldSinceMs = in.nowMs;

            uint32 const heldMs =
                in.nowMs >= v.stopHoldSinceMs ? in.nowMs - v.stopHoldSinceMs : 0;
            if (in.stallMs && heldMs >= in.stallMs && !v.stallReported)
            {
                v.reportStall = true;
                v.stallReported = true;
            }
        }
        else
        {
            v.stopHoldSinceMs = 0;
        }

        // --- 5. what to do about it -------------------------------------------
        switch (v.state)
        {
            case State::Pressure:
            case State::Prelude:
            case State::Advance:
                // Steer. Deliberately unconditional on the wall for Advance: if
                // the leader has moved on, the party is behind her, and every
                // second spent finishing the last ghoul at the OLD stop is a
                // second the next batch — which spawns at him 7.5s after the wall
                // opens and runs to the NEW stop — spends catching up anyway.
                v.travel = true;
                return v;

            case State::Final:
                // WP18 is 131yd from stop 4 and the leader takes ~19 seconds over
                // it. Winter is gone, so nothing is chasing; walk with her and let
                // the boss state end the event.
                v.travel = in.distToStand > in.standLeash;
                v.holdStill = !v.travel;
                return v;

            case State::Fight:
                // The adds run TO the party's stop and open on whoever is nearest
                // the Lich King. There is nothing to walk at and every claimed
                // tick is one the tank does not spend holding them off the healer.
                v.yieldTick = true;
                return v;

            case State::Hold:
            default:
                v.holdStill = true;
                return v;
        }
    }
}

#endif  // _PLAYERBOT_DCHORESCAPEDECISION_H
