/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Log.h"

#include <cstdint>

#include "Creature.h"
#include "InstanceScript.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"

#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSuppressionTransit.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThrottle.h"

// Halls of Reflection's escape — the PER-BOT half of "stay ahead of him".
//
// The driver (hook 35, Overrides/HallsOfReflectionDriver.cpp) steers the TANK.
// DungeonClearEventDueTrigger is leader-only, so for the whole escape the other
// four bots are in the stock combat engine killing the wall's summons, with
// nothing in the module telling them where to stand. This is that.
//
// AND IT IS NOT A LUXURY, because the encounter is built to pull them backwards.
// Every summon batch is cast AT THE LICH KING — behind the party — and each add
// is given 1000 threat on, and told to AttackStart, the player NEAREST HIM, i.e.
// the rearmost bot. The Risen Witch Doctors then stop at 20yd to cast, which
// leaves them standing on HIS side of the party, so a melee finishing one walks
// toward him by design. Meanwhile stock `flee` / `runaway` / `avoid aoe` and
// FleeAction's MoveAway(target, 5) all answer damage by moving AWAY FROM THE
// THING DEALING IT, which here is exactly backwards.
//
// TWO THINGS THE ENCOUNTER DOES TO A BOT THAT ENDS UP BEHIND OR ON TOP OF HIM:
//
//   * Remorseless Winter (69780 -> 69781) ticks 7068 +/- 863 frost every second
//     to everything within 10 yards. That is the largest sustained number in the
//     dungeon by a wide margin.
//   * Every 2 seconds, each alive player with (p.x - lk.x) + (p.y - lk.y) > 20
//     eats 70653 — 10 000 damage AND A KNOCKBACK, which throws the victim
//     further behind and makes the next check worse. The rule is
//     self-reinforcing, so it has to be pre-empted rather than reacted to; this
//     rung fires at a sixth of that scalar.
//
// WHY THE GENERIC HAZARD VACATE CANNOT SERVE. It retreats RADIALLY — a point
// directly away from the emitter, past its pulse radius — and for a bot that is
// already behind him "away" is further behind, into the zap rule. There is one
// correct direction on this leg and it is FORWARD, so the ring is registered as
// a placement keep-out with no vacateRadius (see DcHazardRegistry) and the
// active move is this, one relevance rung above it.

namespace
{
    using namespace DcHallsOfReflection;

    // Everything both halves of this rung need, resolved once.
    //
    // Deliberately NOT split into a trigger probe and an action probe: the two
    // must agree about which stand point the bot is walking to, and re-deriving
    // it from a live Lich King that has moved between the trigger and the action
    // is how a bot ends up walking at last tick's answer.
    struct StayAheadView
    {
        bool  armed = false;      // the escape is running and he is dangerous
        bool  pressured = false;  // ...and THIS bot is inside the ring or behind him
        float distToLk = 0.0f;
        float sumMinusLkSum = 0.0f;
        uint8 targetStop = 1;
        HorPoint stand{};
    };

    StayAheadView Probe(Player* bot)
    {
        StayAheadView v;
        if (!bot || bot->isDead() || bot->GetMapId() != MAP_ID)
            return v;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst || inst->GetBossState(DATA_LICH_KING) != IN_PROGRESS)
            return v;

        Creature* lk = inst->instance->GetCreature(inst->GetGuidData(NPC_LICH_KING));
        if (!lk || !lk->IsAlive())
            return v;

        // GATED ON REMORSELESS WINTER, because that is what gates both of the
        // encounter's own rules. For the ~16 seconds before he casts it, and for
        // ever after the fourth wall removes it, standing near him is merely
        // pointless — and a rung that kept firing then would hijack the last
        // 131yd run to the gunship for no reason.
        if (!lk->HasAura(SPELL_REMORSELESS_WINTER))
            return v;

        v.armed = true;
        v.distToLk = bot->GetExactDist(lk);
        v.sumMinusLkSum = (bot->GetPositionX() + bot->GetPositionY()) -
                          (lk->GetPositionX() + lk->GetPositionY());
        v.pressured = v.distToLk < LK_PRESSURE_DIST || v.sumMinusLkSum > LK_BEHIND_SUM;

        // WHERE FORWARD IS. Taken from the LEADER, exactly as the driver takes it,
        // through the same pure StopIndexNear — the two must reach the same stand
        // point or the tank and its followers would hold different ground fifty
        // yards apart with a Lich King between them.
        //
        // If she is gone the escape is already lost (the walls open only for her
        // channel), but there is still a right direction, and the last stand point
        // this bot can compute is better than none: fall back to the end of the
        // path, which is forward from everywhere.
        Creature* leader = inst->instance->GetCreature(inst->GetGuidData(NPC_LEADER_PART2));
        uint8 const leaderStop =
            leader && leader->IsAlive()
                ? StopIndexNear(leader->GetPositionX(), leader->GetPositionY())
                : static_cast<uint8>(STOP_COUNT - 1);

        v.targetStop = leaderStop < 1 ? uint8(1) : (leaderStop > 5 ? uint8(5) : leaderStop);
        v.stand = StandPointFor(v.targetStop);
        return v;
    }
}

bool DungeonClearHorStayAheadTrigger::IsActive()
{
    // Map first: this is registered in both engines on every bot, and everywhere
    // outside Halls of Reflection it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcHallsOfReflection::MAP_ID)
        return false;

    // Cannot step forward while rooted or stunned — and the Lumbering
    // Abomination's Vomit Spray and the Raging Ghouls' leap both come with
    // control. Do not claim the tick from something that might break it.
    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    StayAheadView const v = Probe(bot);
    return v.armed && v.pressured;
}

bool DungeonClearHorStayAheadAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    StayAheadView const v = Probe(bot);
    if (!v.armed || !v.pressured)
        return false;  // raced clear between the trigger and here

    // ALREADY THERE. The bot is inside the stand point's leash and still reading
    // pressured, which means the Lich King has walked up to the party rather than
    // the party having drifted back to him — the ordinary state of affairs at a
    // wall that is taking a while. There is nowhere better to go, so hand the tick
    // back rather than re-issuing a move to where the bot is standing; the answer
    // to that situation is killing the wall's adds, not moving.
    if (bot->GetExactDist(v.stand.x, v.stand.y, v.stand.z) <= STAND_LEASH)
        return false;

    // Re-issue floor on the destination. The stand point only changes when the
    // leader reaches her next stop, so without this the rung would re-plot the
    // same spline every tick of a wall fight — and TravelTo has no
    // "already moving" guard by design (in combat a bot is essentially always
    // moving under MoveChase, and such a guard would make the whole thing a
    // no-op).
    DcRunState& st = DcRun::Of(botAI);
    if (st.ThrottledIssue(DcThrottle::HorStayAheadIssue, v.stand.x, v.stand.y, v.stand.z,
                          /*epsilon*/ 2.0f, /*windowMs*/ 1500))
        return false;

    // FORWARD, THROUGH THE LONG-RANGE FUNNEL. `forcePath` is unconditional: the
    // legs between stand points run 100-176yd, a bare MovePoint truncates
    // silently past ~30, and TravelTo's own 30yd gate reads the STRAIGHT-LINE
    // distance — which on the stretch from stop 1 to stop 2 understates a
    // corridor that bends twice.
    if (!DcTransit::TravelTo(bot, botAI, v.stand.x, v.stand.y, v.stand.z,
                             DcHallsOfReflection::STAND_LEASH, /*forcePath*/ true))
        return false;

    LOG_DEBUG("playerbots.dungeonclear",
              "[DC:{}] HoR escape — stepping forward to stand {} ({:.1f}yd from the Lich "
              "King, {:+.1f} on the behind-scalar; his ring is {:.0f}yd and the zap lands "
              "at +20)",
              bot->GetName(), v.targetStop, v.distToLk, v.sumMinusLkSum, LK_PRESSURE_DIST);
    return true;
}
