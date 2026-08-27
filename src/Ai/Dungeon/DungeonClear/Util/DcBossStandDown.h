/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCBOSSSTANDDOWN_H
#define _PLAYERBOT_DCBOSSSTANDDOWN_H

#include "Define.h"

class Player;

// Raid boss STAND-DOWN — the non-interference contract of a raid run.
//
// On a raid map, dungeon-clear owns everything BETWEEN fights: it delivers a
// rested, buffed, staged raid to the pull and lets go. The boss fight itself
// belongs to mod-playerbots' raid strategies (mapId-attached, ACTION_RAID=60+
// relevance). While an encounter is live, every DC combat behavior and every
// recovery ladder must go inert — a DC watchdog force-clearing combat or a
// stranded-recovery teleport mid-encounter would sabotage the fight it exists
// to protect. The ONE exemption: encounter events registered for the active
// boss (BWL's Razorgore orb) keep driving through the event executor — that
// orchestration is explicitly DC's job (see DungeonEvent::encounterActive).
//
// Detection is leader-evaluated and memoised per tick: in-encounter when the
// InstanceScript reports IsEncounterInProgress(), or when a roster boss holds
// any member in PvE combat (the no-script fallback). Hysteresis is asymmetric
// by design — ENTER instantly (the first tick of a pull must already be
// gated), EXIT only after the signals have read clear for kExitGraceMs.
// Submerge phases, RP pauses and the Razorgore phase flip all blink the
// signals for a moment; flapping DC back on inside a fight is exactly the
// failure the grace absorbs.
//
// The pure kernel below (Update) is the tested decision; the glue (IsActive)
// resolves the leader, computes the signals and stores the two state fields in
// the leader-owned DcRunState (standDownActive / standDownSignalMs).
namespace DcBossStandDown
{
    // Exit hysteresis: how long BOTH signals must read clear before stand-down
    // releases. ~3s rides out scripted signal gaps without meaningfully
    // delaying the post-kill loot/advance resume.
    constexpr uint32 kExitGraceMs = 3000;

    // Per-tick memo window for the leader evaluation, same contract as
    // DcTickMemo: dedupes strictly WITHIN one AI tick (>=100ms apart), never
    // across ticks.
    constexpr uint32 kEvalWindowMs = 50;

    struct Verdict
    {
        bool   active;
        uint32 lastSignalMs;  // write back to state (0 = never signalled)
    };

    // PURE kernel: fold one observation into the hysteresis state.
    //   signal  — an encounter reads live THIS tick
    //   nowMs   — getMSTime()
    // Enter is instant; exit requires `exitGraceMs` of continuous quiet.
    // Unsigned subtraction keeps the wraparound behavior of getMSTimeDiff.
    inline Verdict Update(bool wasActive, uint32 lastSignalMs, bool signal,
                          uint32 nowMs, uint32 exitGraceMs = kExitGraceMs)
    {
        if (signal)
            return { true, nowMs };
        if (!wasActive)
            return { false, lastSignalMs };
        if (nowMs - lastSignalMs >= exitGraceMs)
            return { false, lastSignalMs };
        return { true, lastSignalMs };
    }

    // Is the stand-down currently holding DC back for `bot`'s run?
    //
    // False everywhere outside raid maps — 5-man boss fights are DC's own to
    // drive. On a raid map the verdict is the LEADER's, evaluated at most once
    // per tick window and read cross-bot by every member (one check in the
    // combat multiplier gates every DC combat action; the recovery ladders and
    // the phantom-combat breaker consult it directly).
    bool IsActive(Player* bot);
}

#endif  // _PLAYERBOT_DCBOSSSTANDDOWN_H
