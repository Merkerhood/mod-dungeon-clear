/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcLeaderSignal.h"

#include "DungeonClearUtil.h"   // DC_PULL_* log macros
#include "DungeonClearMath.h"
#include "DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "AttackersValue.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

namespace
{
    // True only when a COMPLETE navmesh route (PATHFIND_NORMAL) exists from the
    // bot to `p`. The scout-trail picker only returns crumbs the follower can
    // actually reach over a generated path (a trail can span a navmesh seam that
    // is short in plan view yet not walkable straight-line). File-local twin of
    // the same helper in DcPullPlanner.
    bool IsNavReachable(Player* bot, Position const& p)
    {
        if (!bot)
            return false;
        PathGenerator gen(bot);
        gen.CalculatePath(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());
        return gen.GetPathType() == PATHFIND_NORMAL;
    }
}

Player* DcLeaderSignal::FindLeaderTank(Player* reference)
{
    if (!reference)
        return nullptr;

    Group* group = reference->GetGroup();
    if (!group)
    {
        // Solo: a tank bot leads itself; anyone else has no leader.
        return (PlayerbotAI::IsTank(reference) && GET_PLAYERBOT_AI(reference))
                   ? reference : nullptr;
    }

    // Lowest-GUID alive tank bot on the reference's map. GetFirstMember walks
    // every member of the group — including all raid sub-groups — so each member
    // sees the same candidate set and elects the same leader.
    Player* leader = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive())
            continue;
        if (member->GetMapId() != reference->GetMapId())
            continue;
        if (!PlayerbotAI::IsTank(member))
            continue;
        // Only bot tanks can drive — a real-player tank has no PlayerbotAI to
        // run the clear, so it can never be the leader.
        if (!GET_PLAYERBOT_AI(member))
            continue;
        if (!leader || member->GetGUID() < leader->GetGUID())
            leader = member;
    }
    return leader;
}
bool DcLeaderSignal::IsDungeonClearLeader(Player* bot)
{
    return bot && FindLeaderTank(bot) == bot;
}
bool DcLeaderSignal::IsInPausedDungeonClearRun(Player* bot)
{
    if (!bot)
        return false;

    // Resolve the run owner from any member (the leader resolves to itself).
    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* leaderCtx = leaderAI->GetAiObjectContext();
    return leaderCtx->GetValue<bool>("dungeon clear enabled")->Get() &&
           leaderCtx->GetValue<bool>("dungeon clear paused")->Get();
}
bool DcLeaderSignal::IsPullPhaseHolding(uint32 phase)
{
    return phase == static_cast<uint32>(DcPullPhase::Forming) ||
           phase == static_cast<uint32>(DcPullPhase::Advancing) ||
           phase == static_cast<uint32>(DcPullPhase::Returning);
}
bool DcLeaderSignal::GetLeaderPullInfo(Player* bot, uint32& phaseOut, Position& campOut)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return false;

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    // Pull behavior only matters while the run is live, unpaused, and pull mode
    // is on. A paused/off leader holds the whole party via the normal gates.
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (pull.phase == DcPullPhase::Idle)
        return false;

    phaseOut = static_cast<uint32>(pull.phase);
    campOut = pull.camp;
    return true;
}
bool DcLeaderSignal::GetLeaderCampHold(Player* bot, Position& campOut, bool& passiveOut)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; it never holds at its own camp

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get() ||
        !ctx->GetValue<bool>("dungeon clear pull mode")->Get())
        return false;

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    Position const camp = pull.camp;
    // No camp marked yet (pull mode just toggled on, or a reset cleared it): there
    // is nothing to hold at, so let the caller fall back (briefly) to follow.
    if (camp.GetPositionX() == 0.0f && camp.GetPositionY() == 0.0f &&
        camp.GetPositionZ() == 0.0f)
        return false;

    campOut = camp;
    passiveOut = IsPullPhaseHolding(static_cast<uint32>(pull.phase));
    return true;
}
bool DcLeaderSignal::IsLeaderCampFightActive(Player* bot)
{
    if (!bot || bot->isDead())
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader runs the fight; it never assists itself

    uint32 phase = 0;
    Position camp;
    if (!GetLeaderPullInfo(bot, phase, camp))
        return false;

    // Only the camp fight (Engage) — during the holding phases the party is
    // pinned passive at camp by hold-at-camp/stay-at-camp, and a leader-in-combat
    // while merely scouting (Idle) is handled by the drag-back maneuver, which
    // flips the phase out of Idle before any party member would assist.
    return phase == static_cast<uint32>(DcPullPhase::Engage) && leader->IsInCombat();
}
bool DcLeaderSignal::IsLeaderDynamicScouting(Player* bot)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; the lag applies to followers only

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    if (!ctx->GetValue<bool>("dungeon clear enabled")->Get() ||
        ctx->GetValue<bool>("dungeon clear paused")->Get())
        return false;

    // Dynamic mode only (pull setting == 2). Off/On have no scouting-then-decide
    // window — the party either always follows close (Off) or always holds at camp
    // (On) — so the lag would only ever delay them for no benefit there.
    if (ctx->GetValue<uint32>("dungeon clear pull setting")->Get() != 2u)
        return false;

    // Still scouting: the verdict for the upcoming pack hasn't been committed. Once
    // the tank tags the pack it enters combat, and an Advanced verdict marks a camp
    // (handing the party to hold-at-camp) — either way the phase leaves Idle and the
    // party stops lagging and engages.
    if (leader->IsInCombat())
        return false;
    DcPullContext const& pull =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    return pull.phase == DcPullPhase::Idle;
}
bool DcLeaderSignal::GetLeaderScoutTrailPoint(Player* bot, float lag, Position& out)
{
    if (!bot)
        return false;

    Player* leader = FindLeaderTank(bot);
    if (!leader || leader == bot)
        return false;  // the leader drives; only followers trail it

    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return false;

    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    // The trail lives in the LEADER's context — only the tank runs Advance and so
    // only the tank records breadcrumbs.
    std::vector<Position> const& crumbs =
        ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get().breadcrumbs;
    if (crumbs.empty())
        return false;

    Position const tankPos(leader->GetPositionX(), leader->GetPositionY(),
                           leader->GetPositionZ());

    // Walk BACK along the trail (newest -> oldest) accumulating real walked distance,
    // exactly like ComputeTrailCamp, and return the first reachable crumb at least
    // `lag` yards behind the tank. A 3D segment > kJumpGuard is a drag/teleport seam
    // — stop, nothing beyond it is contiguously "behind" the tank. Track the farthest
    // reachable crumb as the fallback when the trail is shorter than the full lag.
    constexpr float kJumpGuard = 12.0f;
    Position best = tankPos;
    float bestAlong = 0.0f;
    bool haveReachable = false;
    Position prev = tankPos;
    float along = 0.0f;
    for (std::size_t i = crumbs.size(); i-- > 0; )
    {
        Position const& c = crumbs[i];
        float const seg = prev.GetExactDist(&c);
        prev = c;
        if (seg > kJumpGuard)
            break;  // discontinuity behind us — stop here
        along += seg;
        // Only ever trail to a crumb the follower can reach over a complete
        // generated path; a crumb across a navmesh seam would straight-line the
        // move under the map.
        if (!IsNavReachable(bot, c))
            continue;
        haveReachable = true;
        if (along > bestAlong)
        {
            best = c;
            bestAlong = along;
        }
        if (along >= lag)
        {
            out = c;
            return true;
        }
    }

    // Trail shorter than the full lag: trail the farthest reachable crumb we found
    // (the follower simply stacks a little closer until more trail accrues).
    if (!haveReachable)
        return false;
    out = best;
    return true;
}
void DcLeaderSignal::AbortLeaderPull(Player* bot)
{
    if (!bot)
        return;
    Player* leader = FindLeaderTank(bot);
    if (!leader)
        return;
    PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
    if (!leaderAI)
        return;
    AiObjectContext* ctx = leaderAI->GetAiObjectContext();
    DcPullContext& pull = ctx->GetValue<DcPullContext&>("dungeon clear pull context")->Get();
    if (IsPullPhaseHolding(static_cast<uint32>(pull.phase)))
    {
        pull.phase = DcPullPhase::Engage;
        DC_PULL_INFO("[DC:{}] advanced-pull: leader pull aborted (forced to Engage) "
                     "-> party released", leader->GetName());
    }
}
void DcLeaderSignal::SetLeaderDazeImmunity(Player* leader, bool apply)
{
    if (!leader)
        return;

    // Block all spells carrying the Daze mechanic (spell 1604 is the only one
    // creatures apply). spellId 0 = a blanket mechanic block. Pair add/remove
    // exactly: remove first so a re-apply never stacks duplicate entries.
    leader->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_DAZE, false);
    if (apply)
    {
        leader->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_DAZE, true);
        // Immunity only blocks FUTURE applications; clear any Daze already on
        // the tank from before pull mode came on (or before the drag started).
        leader->RemoveAurasDueToSpell(1604);
    }
}
