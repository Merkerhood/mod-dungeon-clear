/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestPlanManager.h"

#include <algorithm>
#include <ctime>

#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"

#include "TestRun/DcTestDungeonRegistry.h"
#include "TestRun/DcTestPlanSummary.h"
#include "TestRun/DcTestRunManager.h"

namespace
{
    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::time(nullptr)) * 1000;
    }

    std::string MakePlanId()
    {
        static uint32 counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "tp-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    // How many consecutive transient Start rejections (with nothing in flight
    // to unblock us) before the plan is declared stalled and aborted.
    constexpr uint32 kMaxTransientStreak = 3;
}

DcTestPlanManager& DcTestPlanManager::Instance()
{
    static DcTestPlanManager instance;
    return instance;
}

bool DcTestPlanManager::Start(DcTestPlan::Spec spec, Player* gm, std::string* msg)
{
    auto fail = [&](std::string const& why) -> bool
    {
        if (msg)
            *msg = "Test plan not started: " + why;
        return false;
    };

    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(spec.dungeonToken);
    if (!row)
        return fail("unknown dungeon '" + spec.dungeonToken + "' — see .dc test list");
    spec.dungeonToken = row->token;  // canonicalize a mapId argument to the token

    if (!gm)
        return fail("no issuing player");

    uint32 const maxPlans = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.MaxPlans", 2);
    if (maxPlans != 0 && _plans.size() >= maxPlans)
        return fail("max active test plans reached (" + std::to_string(maxPlans) +
                    ") — .dc test plan stop <planId> first");

    uint32 const maxTotal = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.Plan.MaxTotal", 500);
    if (spec.total > maxTotal)
        return fail("total=" + std::to_string(spec.total) + " exceeds the cap (" +
                    std::to_string(maxTotal) + ", DungeonClear.TestRun.Plan.MaxTotal)");

    // Default + clamp the plan's concurrency to the run manager's global cap so
    // the scheduler isn't asking for launches Start would always reject.
    uint32 const maxConcurrent =
        sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.MaxConcurrent", 8);
    if (spec.concurrent == 0)
        spec.concurrent = std::min<uint32>(5, maxConcurrent ? maxConcurrent : 5);
    if (maxConcurrent != 0)
        spec.concurrent = std::min(spec.concurrent, maxConcurrent);
    spec.concurrent = std::max<uint32>(spec.concurrent, 1);
    spec.concurrent = std::min(spec.concurrent, spec.total);

    spec.planId = MakePlanId();

    Plan plan;
    plan.spec = spec;
    plan.gmGuid = gm->GetGUID();
    plan.startedAtMs = NowUnixMs();
    _plans.push_back(std::move(plan));

    LOG_INFO("playerbots.dungeonclear",
             "TESTPLAN START {} dungeon={} total={} concurrent={} level={} seedBase={} gm={}",
             spec.planId, spec.dungeonToken, spec.total, spec.concurrent, spec.level,
             spec.seedBase, gm->GetName());

    if (msg)
        *msg = Acore::StringFormat("Test plan started: {} {} total={} concurrent={}{}",
                                   spec.planId, spec.dungeonToken, spec.total, spec.concurrent,
                                   spec.seedBase ? Acore::StringFormat(" seedBase={}", spec.seedBase)
                                                 : std::string());
    return true;
}

bool DcTestPlanManager::Stop(std::string const& selector, std::string* msg)
{
    if (_plans.empty())
    {
        if (msg)
            *msg = "no test plan active";
        return false;
    }

    std::vector<Plan*> targets;
    if (selector.empty())
    {
        if (_plans.size() > 1)
        {
            if (msg)
            {
                *msg = "multiple test plans active — specify a planId or 'all':";
                for (Plan const& plan : _plans)
                    *msg += "\n  " + StatusLine(plan);
            }
            return false;
        }
        targets.push_back(&_plans.front());
    }
    else if (selector == "all")
    {
        for (Plan& plan : _plans)
            targets.push_back(&plan);
    }
    else
    {
        for (Plan& plan : _plans)
            if (plan.spec.planId == selector)
                targets.push_back(&plan);
        if (targets.empty())
        {
            if (msg)
            {
                *msg = "no plan matches '" + selector + "' — active plans:";
                for (Plan const& plan : _plans)
                    *msg += "\n  " + StatusLine(plan);
            }
            return false;
        }
    }

    std::string acc;
    for (Plan* plan : targets)
    {
        plan->stopping = true;
        if (plan->result.empty())
            plan->result = "stopped";

        // Abort the live children through the run manager's own selector path;
        // their outcomes flow back via OnRunFinished and the drain finalizes.
        for (std::string const& runId : plan->activeRunIds)
        {
            std::string ignored;
            DcTestRunManager::Instance().Stop(runId, &ignored);
        }

        if (!acc.empty())
            acc += '\n';
        acc += "stopping " + plan->spec.planId + " (" + plan->spec.dungeonToken + ", " +
               std::to_string(plan->counters.activeNow) + " run(s) draining)";
    }
    if (msg)
        *msg = acc;
    return true;
}

void DcTestPlanManager::StopAll(std::string const& reason)
{
    for (Plan& plan : _plans)
    {
        plan.stopping = true;
        if (plan.result.empty())
        {
            plan.result = "stopped";
            plan.abortReason = reason;
        }
    }
}

std::string DcTestPlanManager::StatusLine(Plan const& plan)
{
    DcTestPlan::Counters const& c = plan.counters;
    return Acore::StringFormat("{} {} {}/{} done ({} ok, {} fail), {} active{}",
                               plan.spec.planId, plan.spec.dungeonToken,
                               c.succeeded + c.failed, plan.spec.total, c.succeeded, c.failed,
                               c.activeNow,
                               plan.stopping ? ", draining"
                                             : (plan.backoffMs ? ", backoff" : ""));
}

std::string DcTestPlanManager::StatusText() const
{
    if (_plans.empty())
        return "no test plans active";

    std::string out = std::to_string(_plans.size()) +
                      (_plans.size() == 1 ? " test plan active:" : " test plans active:");
    for (Plan const& plan : _plans)
        out += "\n  " + StatusLine(plan);
    return out;
}

std::vector<DcTestRunLive::PlanSnapshot> DcTestPlanManager::Snapshots() const
{
    std::vector<DcTestRunLive::PlanSnapshot> out;
    out.reserve(_plans.size());
    uint64 const nowMs = NowUnixMs();
    for (Plan const& plan : _plans)
    {
        DcTestRunLive::PlanSnapshot s;
        s.planId = plan.spec.planId;
        s.dungeon = plan.spec.dungeonToken;
        s.total = plan.spec.total;
        s.launched = plan.counters.launched;
        s.succeeded = plan.counters.succeeded;
        s.failed = plan.counters.failed;
        s.activeNow = plan.counters.activeNow;
        s.concurrent = plan.spec.concurrent;
        s.state = plan.stopping ? "draining" : (plan.backoffMs ? "backoff" : "running");
        s.elapsedS = static_cast<uint32>((nowMs - plan.startedAtMs) / 1000);
        out.push_back(std::move(s));
    }
    return out;
}

void DcTestPlanManager::Tick(uint32 diff)
{
    if (_plans.empty())
        return;

    for (Plan& plan : _plans)
        TickPlan(plan, diff);

    for (auto it = _plans.begin(); it != _plans.end();)
    {
        if (DcTestPlan::IsFinished(it->spec, it->counters, it->stopping))
        {
            Finalize(*it);
            it = _plans.erase(it);
        }
        else
            ++it;
    }
}

void DcTestPlanManager::TickPlan(Plan& plan, uint32 diff)
{
    plan.backoffMs = plan.backoffMs > diff ? plan.backoffMs - diff : 0;

    if (plan.stopping)
        return;

    uint32 const wanted = DcTestPlan::LaunchesWanted(
        plan.spec, plan.counters, DcTestRunManager::Instance().CapHeadroom(), plan.backoffMs);
    if (wanted == 0)
        return;

    // At most one launch per world tick per plan: each accepted Start feeds
    // five async bot logins into the shared provisioning budget, and spreading
    // the starts keeps the world tick smooth. The next tick launches the next.
    Player* gm = ObjectAccessor::FindConnectedPlayer(plan.gmGuid);
    uint32 const backoffCfg =
        sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.Plan.BackoffMs", 5000);
    if (!gm)
    {
        // The issuing GM is gone; treat like a transient rejection — they may
        // be relogging (the stall escalation below bounds how long we spin).
        plan.backoffMs = backoffCfg;
        if (plan.counters.activeNow == 0 && ++plan.transientStreak >= kMaxTransientStreak)
            AbortPlan(plan, "stalled: issuing GM logged out");
        return;
    }

    uint32 const seed =
        plan.spec.seedBase ? plan.spec.seedBase + plan.counters.launched : 0;

    std::string msg;
    std::string runId;
    DcTestRunManager::StartErr err = DcTestRunManager::StartErr::None;
    bool const ok = DcTestRunManager::Instance().Start(gm, plan.spec.dungeonToken,
                                                       plan.spec.level, seed, &msg,
                                                       plan.spec.planId, &err, &runId);
    if (ok)
    {
        ++plan.counters.launched;
        ++plan.counters.activeNow;
        plan.activeRunIds.push_back(runId);
        plan.transientStreak = 0;
        LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} launched {} ({}/{})",
                 plan.spec.planId, runId, plan.counters.launched, plan.spec.total);
        return;
    }

    switch (err)
    {
        case DcTestRunManager::StartErr::CapHit:
        case DcTestRunManager::StartErr::BotBudget:
        case DcTestRunManager::StartErr::PoolExhausted:
            plan.backoffMs = backoffCfg;
            // With children in flight a rejection resolves itself when one
            // finishes; only a rejection with nothing running can be a
            // permanent misconfiguration (empty pool, cap 0) — count those.
            if (plan.counters.activeNow == 0 && ++plan.transientStreak >= kMaxTransientStreak)
                AbortPlan(plan, "stalled: " + msg);
            break;
        default:
            AbortPlan(plan, msg);
            break;
    }
}

void DcTestPlanManager::AbortPlan(Plan& plan, std::string const& reason)
{
    plan.stopping = true;
    plan.result = "aborted";
    plan.abortReason = reason;
    LOG_INFO("playerbots.dungeonclear", "TESTPLAN {} aborting: {}", plan.spec.planId, reason);
    for (std::string const& runId : plan.activeRunIds)
    {
        std::string ignored;
        DcTestRunManager::Instance().Stop(runId, &ignored);
    }
}

void DcTestPlanManager::OnRunFinished(std::string const& planId, DcTestPlan::RunOutcome outcome)
{
    for (Plan& plan : _plans)
    {
        if (plan.spec.planId != planId)
            continue;

        auto it = std::find(plan.activeRunIds.begin(), plan.activeRunIds.end(), outcome.runId);
        if (it != plan.activeRunIds.end())
            plan.activeRunIds.erase(it);
        if (plan.counters.activeNow > 0)
            --plan.counters.activeNow;

        if (outcome.result == "success")
            ++plan.counters.succeeded;
        else
            ++plan.counters.failed;
        plan.outcomes.push_back(std::move(outcome));
        return;
    }
}

void DcTestPlanManager::Finalize(Plan& plan)
{
    DcTestPlanSummary::Stats const stats = DcTestPlanSummary::Build(plan.outcomes);

    DcTestPlanSummary::Header h;
    h.planId = plan.spec.planId;
    h.dungeon = plan.spec.dungeonToken;
    if (DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(plan.spec.dungeonToken))
        h.dungeonName = row->name;
    h.total = plan.spec.total;
    h.concurrent = plan.spec.concurrent;
    h.level = plan.spec.level;
    h.seedBase = plan.spec.seedBase;
    h.startedAtMs = plan.startedAtMs;
    h.endedAtMs = NowUnixMs();
    h.durationS = static_cast<uint32>((h.endedAtMs - h.startedAtMs) / 1000);
    h.result = plan.result.empty() ? "completed" : plan.result;
    h.abortReason = plan.abortReason;
    DcTestPlanSummary::Append(h, stats);

    LOG_INFO("playerbots.dungeonclear",
             "TESTPLAN END {} result={} runs={}/{} ok={} fail={} duration={}s{}",
             h.planId, h.result, stats.launched, h.total, stats.succeeded, stats.failed,
             h.durationS, h.abortReason.empty() ? std::string() : (" reason=" + h.abortReason));

    if (Player* gm = ObjectAccessor::FindConnectedPlayer(plan.gmGuid))
        ChatHandler(gm->GetSession()).SendSysMessage(Acore::StringFormat(
            "Test plan {} {}: {} — {}/{} runs succeeded{}{}",
            h.planId, h.dungeon, h.result, stats.succeeded, stats.launched,
            stats.succeeded ? Acore::StringFormat(", median {}s", stats.medianS) : std::string(),
            h.abortReason.empty() ? std::string() : (" — " + h.abortReason)));
}
