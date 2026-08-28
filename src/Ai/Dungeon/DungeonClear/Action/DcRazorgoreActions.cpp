/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "Ai/Dungeon/DungeonClear/Action/DungeonClearActions.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRazorgoreDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"

// Blackwing Lair's two member-side rungs: the orb runner's walk to the ledge,
// and the rest of the raid's camp at the foot of it. See their classes in
// DungeonClearActions.h for what each is and why it acts on its own tick rather
// than being driven by the leader.
//
// There used to be a third — a guards rung that glided EVERY bot to a staging
// point on the orb platform the moment the leader came within 100yd of it. It is
// gone. Grethok the Controller is a boss anchor now, so the raid arrives at that
// platform the way it arrives anywhere else: the advance walks it there as one
// body, the raid muster tops it off, and the tank pulls. A rung that moves forty
// bots is not a substitute for a pull; it is a footrace with the tank in front.

namespace
{
    using namespace DcBlackwingLair;

    // Beyond this a bare MovePoint is not trusted to deliver: the engine
    // PathGenerator caps a generated path at 74 polys / 74 points and truncates
    // silently past that. The walk to the orb is ~78yd from the boss's spawn, so
    // it is a long haul every time. (Same number and same reason as the Violet
    // Hold driver's VH_LONG_HAUL.)
    constexpr float ORB_LONG_HAUL = 30.0f;

    // Same-destination re-issue floor. The orb never moves, so the only reason to
    // re-issue is that something layered a mover over our glide — in combat the
    // stock engine does exactly that whenever it wins a tick.
    constexpr uint32 ORB_REISSUE_MS = 1500;

    // "Is the glide in flight still aimed at the orb?" The destination is a fixed
    // point, so this only has to absorb the couple of yards of slop between the
    // requested point and where the route actually ends.
    constexpr float ORB_REPATH_EPSILON = 3.0f;

    // Below this the bot has no usable bearing from the camp centre (it is
    // effectively on top of it, or straight above it on the ledge) — there is no
    // "near edge" to aim at, so aim at the centre and let the next tick, from a
    // real bearing, do the holding.
    constexpr float CAMP_BEARING_FLOOR = 1.0f;

    // Where to walk a bot the camp wants back: the point on the camp boundary
    // nearest to it, pulled CAMP_HOLD_MARGIN inside — the near EDGE of the camp,
    // never its centre. See CAMP_HOLD_MARGIN in DungeonEventTables.h for the
    // ping-pong that shape exists to kill.
    //
    // Bearing is taken in 2D — the camp is one flat floor (CAMP_Z is a column
    // probe of it), so the destination's height is the floor's, never the bot's
    // own, and a bot that lands on it reads the same distance to the trigger's
    // 3D test as it does to this one. A bot up on the orb ledge that IS out of
    // leash therefore comes down, which is the only direction the camp ever
    // pulls; nothing in this rung walks a bot UP, and nothing holds one there.
    //
    // The one case the near-edge shape cannot serve is a bot with no usable
    // bearing (CAMP_BEARING_FLOOR) — on top of the centre or straight above it,
    // with no "near edge" to pick. It gets the centre, the one point in the camp
    // that is a column probe and certainly walkable.
    void CampHoldPoint(Player* bot, float& hx, float& hy, float& hz)
    {
        hz = CAMP_Z;
        hx = CAMP_X;
        hy = CAMP_Y;

        float const dx = bot->GetPositionX() - CAMP_X;
        float const dy = bot->GetPositionY() - CAMP_Y;
        float const bearing = std::sqrt(dx * dx + dy * dy);
        if (bearing < CAMP_BEARING_FLOOR)
            return;

        // Never further out than the bot already is: the hold point pulls a bot
        // IN, and a bot inside the ring must not be pushed out to sit on it.
        float const hold = std::min(std::max(CAMP_LEASH - CAMP_HOLD_MARGIN, 0.0f), bearing);
        hx = CAMP_X + dx / bearing * hold;
        hy = CAMP_Y + dy / bearing * hold;
    }

    // Walk a bot to a fixed point in the chamber. Returns true when it issued (or
    // is riding) movement, so the caller can own the tick.
    //
    // Shared by both rungs because both have the same problem: a destination tens
    // of yards away across a room the raid is fighting in. The re-issue floor and
    // the glide-in-flight test are keyed to the DESTINATION, not just to the bot,
    // so the camp rung cannot swallow the orb rung's issuance (or the reverse) for
    // a bot that changes jobs mid-fight — which the elected runner does every
    // time the rotation moves on.
    bool RazorgoreTravel(Player* bot, PlayerbotAI* botAI, float tx, float ty, float tz)
    {
        float const dist = bot->GetExactDist(tx, ty, tz);

        MotionMaster* mm = bot->GetMotionMaster();
        float dx, dy, dz;
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
            mm->GetDestination(dx, dy, dz))
        {
            float const ex = dx - tx, ey = dy - ty, ez = dz - tz;
            if (std::sqrt(ex * ex + ey * ey + ez * ez) <= ORB_REPATH_EPSILON)
                return true;  // already gliding here — let it ride, keep the tick
            DcMovement::ResolveEscortConflict(bot);
        }

        {
            struct LastIssue { float x, y, z; uint32 ms; };
            thread_local std::unordered_map<uint32, LastIssue> lastIssue;
            uint32 const guid = bot->GetGUID().GetCounter();
            auto it = lastIssue.find(guid);
            if (it != lastIssue.end())
            {
                LastIssue const& li = it->second;
                float const ddx = li.x - tx, ddy = li.y - ty, ddz = li.z - tz;
                bool const sameDest =
                    std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) <= ORB_REPATH_EPSILON;
                if (sameDest && GetMSTimeDiffToNow(li.ms) < ORB_REISSUE_MS)
                    return true;
            }
            lastIssue[guid] = { tx, ty, tz, getMSTime() };
        }

        if (dist > ORB_LONG_HAUL)
        {
            ChunkedPathfinder::Result const path =
                LongRangePathfinder::Build(bot, tx, ty, tz);
            if (path.reachable && !path.segments.empty())
            {
                // Element 0 is the live position — the escort path[0]=start
                // convention SplinePath expects.
                Movement::PointsArray points;
                points.push_back(
                    G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
                for (PathSegment const& seg : path.segments)
                {
                    // The chamber is one bowl with a ramp up to the ledge, so a
                    // jump leg should never appear; deliver what we have if it does.
                    if (seg.jumpDown || seg.jumpGap)
                        break;
                    for (G3D::Vector3 const& p : seg.polyline)
                    {
                        if (points.size() >= DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS)
                            break;
                        points.push_back(p);
                    }
                }
                if (DcMovement::SplinePath(botAI, points))
                {
                    LOG_DEBUG("playerbots.dungeonclear",
                              "DungeonClear: Razorgore — {} gliding to ({:.0f}, {:.0f}) "
                              "({:.1f}yd, {} pts)",
                              bot->GetName(), tx, ty, dist, uint32(points.size()));
                    return true;
                }
            }
            LOG_DEBUG("playerbots.dungeonclear",
                      "DungeonClear: Razorgore — {} has no long route to ({:.0f}, {:.0f}) "
                      "({:.1f}yd, {}) -> falling back to a point move",
                      bot->GetName(), tx, ty, dist, path.failureReason);
        }

        bot->GetMotionMaster()->MovePoint(0, tx, ty, tz, FORCED_MOVEMENT_NONE,
                                          /*speed*/ 0.0f, /*orientation*/ 0.0f,
                                          /*generatePath*/ true, /*forceDestination*/ false);
        return true;
    }
}

bool DungeonClearRazorgoreOrbTrigger::IsActive()
{
    // Map first: this is registered in both engines on every bot, and everywhere
    // outside Blackwing Lair it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcBlackwingLair::MAP_ID)
        return false;

    // The leader's election is the whole gate — it already checks that the run is
    // live and unpaused, and that this bot is the one member that may take the orb.
    return DcLeaderSignal::IsLeaderRazorgoreRunner(bot);
}

bool DungeonClearRazorgoreOrbAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    Creature* razor = bot->FindNearestCreature(NPC_RAZORGORE, ROOM_SCAN, /*alive*/ true);

    // ALREADY DRIVING HIM — and therefore doing nothing else whatsoever.
    //
    // 19832 is a CHANNEL on the runner, not a fire-and-forget buff, and the whole
    // egg run hangs off it: the moment it drops, Razorgore is freed, the raid is
    // forbidden to kill him (that is a phase-1 wipe), the runner takes a 60s
    // lockout it cannot click through, and someone else has to start over. Every
    // ordinary thing a DPS bot does with its own body ends that channel — a cast,
    // a wand shot, a melee swing, a step out of a cone.
    //
    // So this branch OWNS the tick and spends it on nothing. That is the exact
    // inverse of what it used to do (hand the tick back "so the rotation runs"),
    // and the rotation running was the bug: the runner would break its own
    // possession mid-run, take the exhaustion, and the raid would lose a window.
    // Ninety seconds of a muted DPS is the PRICE of the mechanic, not a cost this
    // module gets to optimise away — a human raid pays it too.
    //
    // Note there is no drift clause any more either. Walking a channeling bot back
    // to its station is the same interruption as any other movement, and the charm
    // roots the charmer regardless, so the old "reclaim the tick on drift" branch
    // could only ever fire by breaking the thing it was protecting.
    if (razor && razor->IsCharmed() && razor->GetCharmerGUID() == bot->GetGUID())
    {
        // Drop the victim rather than merely declining to pick one. Autoattack is
        // driven off GetVictim() inside Unit::Update, not off the action engine,
        // so owning the tick alone does not stop a swing that a `dps assist` two
        // ticks ago already queued up.
        if (bot->GetVictim())
            bot->AttackStop();

        // Same reasoning for movement: a spline issued before the click keeps
        // delivering after it. Only touched when something is actually in flight —
        // an unconditional stop every tick is a stop packet every tick.
        if (bot->isMoving())
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);

        return true;
    }

    if (bot->GetExactDist2d(ORB_X, ORB_Y) > ORB_STATION_RADIUS)
        return RazorgoreTravel(bot, botAI, ORB_X, ORB_Y, ORB_Z);

    // Standing at the orb. Settle before clicking — a bot still coasting out of a
    // spline is moving, and the script's mind-control cast is an ordinary cast
    // that a moving, casting bot can lose.
    DcMovement::StopBot(bot, DcMovement::Stop::Hold);

    // The orb script's own refusals, re-read live rather than remembered: a pet
    // resummoned on the walk over and a lockout taken thirty seconds ago look
    // identical to a cached flag.
    if (bot->GetPet() || bot->HasAura(SPELL_MIND_EXHAUSTION))
        return false;  // the leader's next election will move on to someone else

    if (!razor || razor->IsCharmed())
        return false;  // nothing to take, or somebody already has him

    // THE SAME GATE THE LEADER'S FSM APPLIES (DcRazorgore::Decide's WaitPull
    // branch), because the runner arrives here on its own tick and would
    // otherwise click an orb the leader believes is still being held back.
    //
    // The gate is the PULL, not the platform. The three level-62 elites are
    // 6-10yd from the orb; a possession taken while they are quiet is one bot
    // face-pulling them alone, and a possession taken once the raid has them is
    // the whole point of the encounter — Razorgore is in Grethok's
    // creature_formation (groupAI 7), so the tag drags him in from across the
    // chamber and every second after it is a second the raid spends damaging a
    // boss whose phase-1 death casts 20038 and instakills all forty of them.
    {
        DcBlackwingLair::OrbGuardState const guards = DcBlackwingLair::OrbGuards(bot);
        if (guards.alive && !guards.engaged && !razor->IsInCombat())
            return true;  // parked and waiting — own the tick, click nothing
    }

    GameObject* orb = bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, ROOM_SCAN);
    if (!orb)
        return false;

    // A cast in flight is ONE call to cancel, and it has to be cancelled here.
    // go_orb_of_domination::GossipHello ends in an ordinary, non-triggered
    // player->CastSpell(razor, 19832), which SPELL_FAILED_SPELL_IN_PROGRESS
    // refuses outright while another cast is up — silently, from our side, since
    // GameObject::Use returns nothing. A DPS bot is casting most ticks, so a
    // runner that arrives mid-rotation used to stand on the orb clicking a button
    // that never took: the leader read "cannot click", elected someone else three
    // seconds later, and the rotation cycled fresh runners for the whole window.
    // (That is also why "is casting" is no longer one of the election's gates —
    // it was disqualifying almost every candidate almost every tick.)
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);

    LOG_INFO("playerbots.dungeonclear",
             "DungeonClear: Razorgore — {} clicks the Orb of Domination", bot->GetName());
    orb->Use(bot);
    return true;
}

bool DungeonClearRazorgoreCampTrigger::IsActive()
{
    // Map first: registered on every bot's engines, both of them, and everywhere
    // outside Blackwing Lair it must cost one integer compare.
    if (!bot || bot->isDead() || bot->GetMapId() != DcBlackwingLair::MAP_ID)
        return false;

    // Only while the egg run is actually being driven. The leader stamps that
    // from the tick its pull on Grethok lands until the thirtieth egg, so this
    // arms with the fight and releases within a tick or two of the end of it —
    // no latch, nothing to reset after a wipe. Before the pull it is inert, and
    // deliberately: the approach belongs to the walk-in and the raid muster.
    if (!DcLeaderSignal::IsLeaderRazorgoreDriving(bot))
        return false;

    // The orb runner is exempt: its whole job is to be somewhere else.
    if (DcLeaderSignal::IsLeaderRazorgoreRunner(bot))
        return false;

    // Distance only, and DELIBERATELY no floor test: the orb ledge — and
    // Grethok's own spawn 18yd from the camp centre — sits inside the 30yd
    // leash, so the guard fight the tank just pulled happens INSIDE the camp and
    // nobody is walked off the pack they are killing. A raid still up there when
    // the egg run starts reads as in position and is left alone; the adds pull it
    // down within the first wave anyway, and a rung that marched everyone off the
    // platform would be a lap across the room for no gain. What the camp
    // guarantees is only that nothing HOLDS the raid up there — this rung never
    // walks a bot up.
    //
    // In position it goes INERT entirely rather than owning the tick and
    // returning false, so the combat engine is never even in contention for a bot
    // that is already where it belongs.
    //
    // One leash for the whole raid, and the trigger and the walk-back read the
    // same number: the trigger decides "is it out", CampHoldPoint decides "where
    // is just-inside", and a mismatch between the two is a rung that either never
    // releases or never fires.
    return bot->GetExactDist(DcBlackwingLair::CAMP_X, DcBlackwingLair::CAMP_Y,
                             DcBlackwingLair::CAMP_Z) > DcBlackwingLair::CAMP_LEASH;
}

bool DungeonClearRazorgoreCampAction::Execute(Event /*event*/)
{
    if (!bot || !botAI)
        return false;

    // The near edge of the camp, NOT its centre — the walk-back is a step back
    // inside the boundary the bot just left. See CAMP_HOLD_MARGIN.
    float hx, hy, hz;
    CampHoldPoint(bot, hx, hy, hz);

    return RazorgoreTravel(bot, botAI, hx, hy, hz);
}
