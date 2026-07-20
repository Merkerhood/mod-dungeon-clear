/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcDiagSnapshot.h"

#include <optional>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"

#include "PlayerbotAI.h"
#include "Playerbots.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcRunState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "TestRun/DcTestRunRecord.h"

namespace
{
    // Split the publisher's tab-separated STATUS frame. Same shape the run job
    // already parses (DcTestRunJob::OnStatusPayload) — reused rather than
    // re-deriving the state ladder, so the snapshot and the status timeline can
    // never disagree about what state the run was in.
    std::vector<std::string> SplitStatus(std::string const& payload)
    {
        std::vector<std::string> parts;
        std::size_t from = 0;
        while (from <= payload.size())
        {
            std::size_t const tab = payload.find('\t', from);
            if (tab == std::string::npos)
            {
                parts.push_back(payload.substr(from));
                break;
            }
            parts.push_back(payload.substr(from, tab - from));
            from = tab + 1;
        }
        return parts;
    }

    std::uint32_t PowerPct(Player* p, Powers power)
    {
        std::uint32_t const maxPower = p->GetMaxPower(power);
        if (!maxPower)
            return 0;
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(p->GetPower(power)) * 100) / maxPower);
    }

    // Wrap-safe "how long ago", clamped: a zero stamp means "never", not "forever".
    std::uint32_t SinceMs(std::uint32_t stampMs)
    {
        if (!stampMs)
            return 0;
        return getMSTimeDiff(stampMs, getMSTime());
    }

    void AppendEscaped(std::ostringstream& s, std::string const& v)
    {
        s << '"' << DcTestRunRecord::EscapeJson(v) << '"';
    }

    void AppendBool(std::ostringstream& s, char const* key, bool v)
    {
        s << ",\"" << key << "\":" << (v ? "true" : "false");
    }

    // DcStrategyGate keeps these file-static in its own TU, so they are
    // duplicated rather than shared. Kept adjacent to that comment so a rename
    // there is caught here: a wrong name silently reports "no DC strategy" on
    // every member, which reads as a real (and very misleading) finding.
    char const* const kDcNonCombatStrategy = "dungeon clear";
    char const* const kDcCombatStrategy    = "dungeon clear combat";
}

namespace DcDiag
{
    Snapshot Capture(Player* tank, char const* capturedAt)
    {
        Snapshot snap;
        snap.capturedAt = capturedAt ? capturedAt : "";

        PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
        if (!tank || !tankAI || !tank->IsInWorld())
            return snap;

        AiObjectContext* context = tankAI->GetAiObjectContext();
        if (!context)
            return snap;

        snap.valid = true;

        // --- run switches -------------------------------------------------
        DcRunState const& rs = DcRun::Of(context);
        snap.enabled = rs.enabled;
        snap.paused = rs.paused;
        snap.pauseReason = rs.pauseReason;
        snap.pausedAtDoor = !rs.pausedDoor.IsEmpty();
        snap.selectedBossEntry = rs.selectedBossEntry;
        snap.smartRestLatched = rs.smartRestLatched;

        // --- state-machine position ---------------------------------------
        snap.phase = AI_VALUE(std::string&, DcKey::Phase);
        snap.stallReason = AI_VALUE(std::string&, DcKey::StallReason);

        std::vector<std::string> const status = SplitStatus(DcStatusPublisher::BuildStatusPayload(tankAI));
        if (status.size() >= 8 && status[0] == "STATUS")
        {
            snap.stateStr = status[6];
            snap.detail = status[7];
        }

        // --- world / tank -------------------------------------------------
        snap.mapId = tank->GetMapId();
        snap.instanceId = tank->GetInstanceId();
        snap.tankX = tank->GetPositionX();
        snap.tankY = tank->GetPositionY();
        snap.tankZ = tank->GetPositionZ();
        snap.tankInCombat = tank->IsInCombat();
        snap.tankMoving = tank->isMoving();
        if (Unit* victim = tank->GetVictim())
            snap.tankVictim = victim->GetName();

        InstanceScript* inst = DcTargeting::GetInstanceScript(tank);
        snap.completedEncounterMask = inst ? inst->GetCompletedEncounterMask() : 0u;

        std::unordered_set<std::uint32_t> const& cleared =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::ClearedAnchors);
        std::unordered_set<std::uint32_t> const& skipped =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::Skipped);
        std::unordered_set<std::uint32_t> const& seen =
            AI_VALUE(std::unordered_set<uint32>&, DcKey::SeenBosses);
        snap.clearedAnchors = static_cast<std::uint32_t>(cleared.size());
        snap.skippedCount = static_cast<std::uint32_t>(skipped.size());

        // --- target -------------------------------------------------------
        snap.stickyBoss = AI_VALUE(uint32, DcKey::StickyBoss);
        std::optional<DungeonBossInfo> const next =
            context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        if (next.has_value())
        {
            snap.nextBossEntry = next->entry;
            snap.nextBossName = next->name;
            if (next->mapId == snap.mapId)
                snap.distToTarget = tank->GetDistance(next->x, next->y, next->z);
        }

        DcApproachState const& appr = AI_VALUE(DcApproachState&, DcKey::ApproachState);
        snap.committedTargetEntry = appr.lastTargetEntry;
        // The cache key of the built route — i.e. the boss the current path
        // actually leads to, which is NOT the same notion as lastTargetEntry
        // (that one is the committed target, the same notion as StickyBoss).
        snap.approachTargetEntry = appr.longPathTargetEntry;

        // Three independent notions of "the boss we are going to" that must
        // agree: the committed target, the recomputed target, and the one the
        // built route actually leads to. A disagreement here is the signature
        // of the run chasing a target it has no path for — worth flagging
        // explicitly rather than making a reader diff three numbers.
        snap.targetMismatch =
            (snap.nextBossEntry && snap.stickyBoss && snap.nextBossEntry != snap.stickyBoss) ||
            (snap.nextBossEntry && snap.committedTargetEntry &&
             snap.nextBossEntry != snap.committedTargetEntry) ||
            (snap.nextBossEntry && snap.approachTargetEntry &&
             snap.nextBossEntry != snap.approachTargetEntry);

        // --- route --------------------------------------------------------
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, DcKey::LongPath);
        snap.pathReachable = path.reachable;
        snap.pathComplete = path.complete;
        snap.pathStartFarFromPoly = path.startFarFromPoly;
        snap.pathFailureReason = path.failureReason;
        snap.pathSegments = static_cast<std::uint32_t>(path.segments.size());

        DungeonFollowerState const& follower =
            AI_VALUE(DungeonFollowerState&, DcKey::FollowerState);
        snap.segmentIdx = follower.segmentIdx;
        snap.pointIdx = follower.pointIdx;
        snap.offPathTicks = follower.offPathTicks;
        // RouteDeviation returns 0 when the cursor is past the path end, which
        // is indistinguishable from "dead on the corridor" — and a cursor past
        // the end is exactly the path-ends-short livelock. Only record a
        // deviation when there is a real point to measure against.
        if (!path.segments.empty() &&
            DungeonPathFollower::CurrentPoint(path, follower).has_value())
            snap.routeDeviation = DungeonPathFollower::RouteDeviation(tank, path, follower);
        else
            snap.cursorPastPathEnd = !path.segments.empty();

        // --- wedge watchdogs ----------------------------------------------
        snap.routeGlideStuck = appr.routeGlideWatch.stuckTicks;
        snap.doorWalkInStuck = appr.doorWalkInWatch.stuckTicks;
        snap.pursuitStuck = appr.pursuitWatch.stuckTicks;
        snap.finalApproachStuck = appr.finalApproachWatch.stuckTicks;
        snap.stuckCount = appr.stuckCount;
        snap.rebuildAttempts = appr.rebuildAttempts;
        snap.resnapAttempts = appr.resnapAttempts;
        snap.partyNotReadyTicks = appr.partyNotReadyTicks;

        snap.doorStalled = !appr.doorStallGuid.IsEmpty();
        snap.doorStalledForMs = snap.doorStalled ? SinceMs(appr.doorStallSinceMs) : 0u;

        // --- pull ---------------------------------------------------------
        DcPullContext const& pull = AI_VALUE(DcPullContext&, DcKey::PullContext);
        snap.pullSetting = AI_VALUE(uint32, DcKey::PullSetting);
        snap.pullPhase = static_cast<std::uint32_t>(pull.phase);
        snap.pullDecision = static_cast<std::uint32_t>(pull.decision);
        snap.pullPhaseForMs = SinceMs(pull.phaseSince);
        snap.pullFizzleCount = pull.fizzleCount;
        snap.pullHasCamp = pull.HasCamp();

        // --- party --------------------------------------------------------
        // GetFirstMember walks the whole raid, which is what we want: a member
        // that fell out of the tank's sub-group still matters to the report.
        if (Group* group = tank->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member)
                    continue;

                MemberSnapshot m;
                m.name = member->GetName();
                m.guid = member->GetGUID().GetRawValue();
                m.level = member->GetLevel();
                m.online = true;
                m.isBot = GET_PLAYERBOT_AI(member) != nullptr;
                m.mapId = member->GetMapId();
                m.x = member->GetPositionX();
                m.y = member->GetPositionY();
                m.z = member->GetPositionZ();
                m.distToTank = (m.mapId == snap.mapId) ? tank->GetDistance(member) : -1.f;
                m.alive = member->IsAlive();
                m.healthPct = static_cast<std::uint32_t>(member->GetHealthPct());
                if (member->getPowerType() == POWER_MANA)
                    m.manaPct = PowerPct(member, POWER_MANA);
                m.inCombat = member->IsInCombat();
                if (Unit* victim = member->GetVictim())
                    m.victim = victim->GetName();

                if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
                {
                    m.dcStrategy = memberAI->HasStrategy(kDcNonCombatStrategy, BOT_STATE_NON_COMBAT);
                    m.dcCombatStrategy = memberAI->HasStrategy(kDcCombatStrategy, BOT_STATE_COMBAT);
                }

                if (m.alive)
                    ++snap.aliveCount;
                if (m.inCombat)
                    ++snap.inCombatCount;
                snap.members.push_back(std::move(m));
            }

            // Members that logged out / were never resolved never appear above;
            // recording them as offline rows is the difference between "the
            // healer died" and "the healer is not in the world any more".
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                bool found = false;
                for (MemberSnapshot const& m : snap.members)
                    if (m.guid == slot.guid.GetRawValue())
                    {
                        found = true;
                        break;
                    }
                if (found)
                    continue;

                // Group::MemberSlot carries no level — an offline row reports
                // level 0, which the reader should treat as "unknown", not "1".
                MemberSnapshot m;
                m.name = slot.name;
                m.guid = slot.guid.GetRawValue();
                m.online = false;
                m.distToTank = -1.f;
                ++snap.offlineCount;
                snap.members.push_back(std::move(m));
            }
        }
        snap.partySize = static_cast<std::uint32_t>(snap.members.size());

        // --- roster -------------------------------------------------------
        // Completion is derived exactly as NextDungeonBossValue derives it —
        // all three paths — and doneVia records WHICH one fired. That is the
        // field that distinguishes a boss the watchdog can see from one it
        // cannot: only "mask" and "anchor" completions reset the no-progress
        // timer, so a roster full of "bossState" kills explains a no_progress
        // verdict on a run that was actually going fine.
        std::vector<DungeonBossInfo> const& bosses =
            AI_VALUE(std::vector<DungeonBossInfo>, DcKey::DungeonBosses);
        for (DungeonBossInfo const& info : bosses)
        {
            BossSnapshot b;
            b.entry = info.entry;
            b.orderKey = BossOrderKey(info);
            b.name = info.name;
            b.kind = (info.kind == DungeonAnchorKind::Boss) ? "boss" : "objective";
            b.encounterIndex = static_cast<std::int32_t>(info.encounterIndex);
            b.x = info.x;
            b.y = info.y;
            b.z = info.z;
            b.isTarget = (snap.nextBossEntry && info.entry == snap.nextBossEntry);
            b.isSticky = (snap.stickyBoss && info.entry == snap.stickyBoss);

            bool done = false;
            if (cleared.count(info.entry))
            {
                done = true;
                b.doneVia = "anchor";
            }
            else if (info.kind == DungeonAnchorKind::Boss && info.encounterIndex < 32 &&
                     (snap.completedEncounterMask & (1u << info.encounterIndex)))
            {
                done = true;
                b.doneVia = "mask";
            }
            else if (info.kind == DungeonAnchorKind::Boss && info.doneBossStateIndex >= 0 && inst &&
                     inst->GetBossState(static_cast<uint32>(info.doneBossStateIndex)) == DONE)
            {
                done = true;
                b.doneVia = "bossState";
            }

            // Same ladder DcBossesAction paints the panel rows with, so the
            // report and the in-game boss list can never disagree.
            if (done)
                b.status = "dead";
            else if (skipped.count(info.entry))
                b.status = "skipped";
            else if (DcTargeting::FindLiveCreatureOnMap(tank, info.entry))
                b.status = "alive";
            else if (DcTargeting::IsCreaturePresentOnMap(tank, info.entry))
                b.status = "dead";  // corpse on the map, completion not latched yet
            else if (seen.count(info.entry))
                b.status = "missing";
            else
                b.status = "alive";  // never seen: grid most likely not loaded

            snap.roster.push_back(std::move(b));
        }

        return snap;
    }

    void AppendJson(std::ostringstream& s, Snapshot const& snap)
    {
        s << "{\"valid\":" << (snap.valid ? "true" : "false") << ",\"capturedAt\":";
        AppendEscaped(s, snap.capturedAt);
        if (!snap.valid)
        {
            s << '}';
            return;
        }

        AppendBool(s, "enabled", snap.enabled);
        AppendBool(s, "paused", snap.paused);
        s << ",\"pauseReason\":";
        AppendEscaped(s, snap.pauseReason);
        AppendBool(s, "pausedAtDoor", snap.pausedAtDoor);
        s << ",\"selectedBossEntry\":" << snap.selectedBossEntry;
        AppendBool(s, "smartRestLatched", snap.smartRestLatched);

        s << ",\"phase\":";
        AppendEscaped(s, snap.phase);
        s << ",\"state\":";
        AppendEscaped(s, snap.stateStr);
        s << ",\"detail\":";
        AppendEscaped(s, snap.detail);
        s << ",\"stallReason\":";
        AppendEscaped(s, snap.stallReason);

        s << ",\"target\":{\"sticky\":" << snap.stickyBoss
          << ",\"nextEntry\":" << snap.nextBossEntry
          << ",\"nextName\":";
        AppendEscaped(s, snap.nextBossName);
        s << ",\"committedEntry\":" << snap.committedTargetEntry
          << ",\"approachEntry\":" << snap.approachTargetEntry
          << ",\"distance\":" << snap.distToTarget
          << ",\"mismatch\":" << (snap.targetMismatch ? "true" : "false") << '}';

        s << ",\"route\":{\"reachable\":" << (snap.pathReachable ? "true" : "false")
          << ",\"complete\":" << (snap.pathComplete ? "true" : "false")
          << ",\"startFarFromPoly\":" << (snap.pathStartFarFromPoly ? "true" : "false")
          << ",\"failureReason\":";
        AppendEscaped(s, snap.pathFailureReason);
        s << ",\"segments\":" << snap.pathSegments
          << ",\"segmentIdx\":" << snap.segmentIdx
          << ",\"pointIdx\":" << snap.pointIdx
          << ",\"offPathTicks\":" << snap.offPathTicks
          << ",\"deviation\":" << snap.routeDeviation
          << ",\"cursorPastPathEnd\":" << (snap.cursorPastPathEnd ? "true" : "false") << '}';

        s << ",\"watchdogs\":{\"routeGlide\":" << snap.routeGlideStuck
          << ",\"doorWalkIn\":" << snap.doorWalkInStuck
          << ",\"pursuit\":" << snap.pursuitStuck
          << ",\"finalApproach\":" << snap.finalApproachStuck
          << ",\"stuckCount\":" << snap.stuckCount
          << ",\"rebuildAttempts\":" << snap.rebuildAttempts
          << ",\"resnapAttempts\":" << snap.resnapAttempts
          << ",\"partyNotReadyTicks\":" << snap.partyNotReadyTicks
          << ",\"doorStalled\":" << (snap.doorStalled ? "true" : "false")
          << ",\"doorStalledForMs\":" << snap.doorStalledForMs << '}';

        s << ",\"pull\":{\"setting\":" << snap.pullSetting
          << ",\"phase\":" << snap.pullPhase
          << ",\"decision\":" << snap.pullDecision
          << ",\"phaseForMs\":" << snap.pullPhaseForMs
          << ",\"fizzleCount\":" << snap.pullFizzleCount
          << ",\"hasCamp\":" << (snap.pullHasCamp ? "true" : "false") << '}';

        s << ",\"world\":{\"map\":" << snap.mapId
          << ",\"instance\":" << snap.instanceId
          << ",\"x\":" << snap.tankX << ",\"y\":" << snap.tankY << ",\"z\":" << snap.tankZ
          << ",\"inCombat\":" << (snap.tankInCombat ? "true" : "false")
          << ",\"moving\":" << (snap.tankMoving ? "true" : "false")
          << ",\"victim\":";
        AppendEscaped(s, snap.tankVictim);
        s << ",\"completedEncounterMask\":" << snap.completedEncounterMask
          << ",\"clearedAnchors\":" << snap.clearedAnchors
          << ",\"skipped\":" << snap.skippedCount << '}';

        s << ",\"party\":{\"size\":" << snap.partySize
          << ",\"alive\":" << snap.aliveCount
          << ",\"offline\":" << snap.offlineCount
          << ",\"inCombat\":" << snap.inCombatCount
          << ",\"members\":[";
        for (std::size_t i = 0; i < snap.members.size(); ++i)
        {
            MemberSnapshot const& m = snap.members[i];
            if (i)
                s << ',';
            s << "{\"name\":";
            AppendEscaped(s, m.name);
            s << ",\"guid\":" << m.guid
              << ",\"level\":" << m.level
              << ",\"bot\":" << (m.isBot ? "true" : "false")
              << ",\"online\":" << (m.online ? "true" : "false")
              << ",\"map\":" << m.mapId
              << ",\"x\":" << m.x << ",\"y\":" << m.y << ",\"z\":" << m.z
              << ",\"distToTank\":" << m.distToTank
              << ",\"alive\":" << (m.alive ? "true" : "false")
              << ",\"hp\":" << m.healthPct
              << ",\"mp\":" << m.manaPct
              << ",\"inCombat\":" << (m.inCombat ? "true" : "false")
              << ",\"victim\":";
            AppendEscaped(s, m.victim);
            s << ",\"dcStrategy\":" << (m.dcStrategy ? "true" : "false")
              << ",\"dcCombatStrategy\":" << (m.dcCombatStrategy ? "true" : "false")
              << '}';
        }
        s << "]}";

        s << ",\"roster\":[";
        for (std::size_t i = 0; i < snap.roster.size(); ++i)
        {
            BossSnapshot const& b = snap.roster[i];
            if (i)
                s << ',';
            s << "{\"entry\":" << b.entry
              << ",\"order\":" << b.orderKey
              << ",\"name\":";
            AppendEscaped(s, b.name);
            s << ",\"kind\":";
            AppendEscaped(s, b.kind);
            s << ",\"status\":";
            AppendEscaped(s, b.status);
            s << ",\"doneVia\":";
            AppendEscaped(s, b.doneVia);
            s << ",\"encounterIndex\":" << b.encounterIndex
              << ",\"x\":" << b.x << ",\"y\":" << b.y << ",\"z\":" << b.z
              << ",\"isTarget\":" << (b.isTarget ? "true" : "false")
              << ",\"isSticky\":" << (b.isSticky ? "true" : "false")
              << '}';
        }
        s << "]}";
    }

    std::string Summarize(Snapshot const& snap)
    {
        if (!snap.valid)
            return "diag: tank unresolvable at capture time";

        std::ostringstream s;
        s.precision(1);
        s << std::fixed;
        s << "state=" << (snap.stateStr.empty() ? "?" : snap.stateStr)
          << " phase=" << (snap.phase.empty() ? "-" : snap.phase)
          << " boss=" << (snap.nextBossName.empty() ? "none" : snap.nextBossName)
          << "(" << snap.nextBossEntry << ")"
          << " dist=" << snap.distToTarget
          << " pos=" << snap.mapId << ":" << snap.tankX << "," << snap.tankY << "," << snap.tankZ
          << " party=" << snap.aliveCount << "/" << snap.partySize << " alive"
          << (snap.offlineCount ? " offline=" + std::to_string(snap.offlineCount) : "")
          << " combat=" << snap.inCombatCount
          << " route=" << (snap.pathReachable ? "ok" : "UNREACHABLE")
          << "/" << snap.pathSegments << "seg"
          << " dev=" << snap.routeDeviation
          << " stuck=" << snap.routeGlideStuck << "/" << snap.pursuitStuck
          << "/" << snap.finalApproachStuck
          << " resnaps=" << snap.resnapAttempts;
        if (snap.doorStalled)
            s << " DOOR-STALLED " << (snap.doorStalledForMs / 1000) << "s";
        if (snap.targetMismatch)
            s << " TARGET-MISMATCH sticky=" << snap.stickyBoss
              << " approach=" << snap.approachTargetEntry;
        if (!snap.pathFailureReason.empty())
            s << " pathFail=\"" << snap.pathFailureReason << "\"";
        if (!snap.stallReason.empty())
            s << " stall=\"" << snap.stallReason << "\"";
        return s.str();
    }
}
