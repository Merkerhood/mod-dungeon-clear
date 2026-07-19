/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestRunManager.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "DcStrategyGate.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "TestRun/DcTestComp.h"
#include "TestRun/DcTestDungeonRegistry.h"

namespace
{
    // Stage timeouts: a setup stage overrunning these is itself the failure.
    constexpr uint32 SPAWN_TIMEOUT_MS = 60 * 1000;
    constexpr uint32 PROVISION_TIMEOUT_MS = 60 * 1000;
    constexpr uint32 GROUP_TIMEOUT_MS = 30 * 1000;
    constexpr uint32 TELEPORT_TIMEOUT_MS = 30 * 1000;
    constexpr uint32 START_TIMEOUT_MS = 20 * 1000;

    constexpr uint32 MONITOR_STEP_MS = 1000;

    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::time(nullptr)) * 1000;
    }

    std::string MakeRunId()
    {
        static uint32 counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "tr-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    char const* ClassToken(uint8 classId)
    {
        switch (classId)
        {
            case 1:  return "warrior";
            case 2:  return "paladin";
            case 3:  return "hunter";
            case 4:  return "rogue";
            case 5:  return "priest";
            case 6:  return "deathknight";
            case 7:  return "shaman";
            case 8:  return "mage";
            case 9:  return "warlock";
            case 11: return "druid";
        }
        return "unknown";
    }

    // Premade-spec template index for the wanted spec name — exact match
    // first, then substring fallback ("prot" catches a renamed "prot pve").
    // -1 when the class has no matching template.
    int ResolveSpecNo(uint8 classId, char const* exact, char const* fallback,
                      std::string* pickedName)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < MAX_SPECNO; ++i)
            {
                std::string const& name = sPlayerbotAIConfig.premadeSpecName[classId][i];
                if (name.empty())
                    break;
                bool const hit = pass == 0 ? name == exact
                                           : name.find(fallback) != std::string::npos;
                if (hit)
                {
                    if (pickedName)
                        *pickedName = name;
                    return i;
                }
            }
        return -1;
    }
}

DcTestRunManager& DcTestRunManager::Instance()
{
    static DcTestRunManager instance;
    return instance;
}

char const* DcTestRunManager::StageName(Stage s)
{
    switch (s)
    {
        case Stage::Idle:         return "idle";
        case Stage::SpawningBots: return "spawning_bots";
        case Stage::Provisioning: return "provisioning";
        case Stage::Grouping:     return "grouping";
        case Stage::Teleporting:  return "teleporting";
        case Stage::Starting:     return "starting";
        case Stage::Monitoring:   return "monitoring";
        case Stage::TearingDown:  return "tearing_down";
    }
    return "?";
}

void DcTestRunManager::EnterStage(Stage s)
{
    _stage = s;
    _stageMs = 0;
}

Player* DcTestRunManager::FindGm() const
{
    // FindConnectedPlayer, NOT FindPlayer: the GM teleporting into the
    // instance to watch sits on a loading screen for a few ticks — not in
    // world, but very much still logged in. FindPlayer() returned null there,
    // and the liveness check below read it as a logout and aborted every run
    // the moment the GM zoned in. Session-based lookup survives the loading
    // screen; a real logout still goes null as soon as the session closes.
    return ObjectAccessor::FindConnectedPlayer(_gmGuid);
}

Player* DcTestRunManager::FindTank() const
{
    return ObjectAccessor::FindPlayer(_tankGuid);
}

bool DcTestRunManager::Start(Player* gm, std::string const& dungeonToken,
                             uint32 levelOverride, std::string* err)
{
    if (_stage != Stage::Idle)
    {
        if (err)
            *err = "a test run is already active (" + _record.dungeon + ", stage " +
                   StageName(_stage) + ") — .dc test stop first";
        return false;
    }

    DcTestDungeonRegistry::Row const* row = DcTestDungeonRegistry::Find(dungeonToken);
    if (!row)
    {
        if (err)
            *err = "unknown dungeon '" + dungeonToken + "' — see .dc test list";
        return false;
    }

    if (!gm || !GET_PLAYERBOT_MGR(gm))
    {
        if (err)
            *err = "no playerbot manager on this account";
        return false;
    }

    ResetState();

    _dungeonToken = row->token;
    _mapId = row->mapId;
    _x = row->x;
    _y = row->y;
    _z = row->z;
    _o = row->o;
    _level = levelOverride ? std::min<uint32>(levelOverride, 80u) : row->recommendedLevel;
    _gmGuid = gm->GetGUID();

    _limits.pauseGraceMs = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.PauseGraceS", 60) * 1000;
    _limits.stallGraceMs = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.StallGraceS", 120) * 1000;
    _limits.noProgressMs = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.NoProgressS", 600) * 1000;
    _limits.overallTimeoutMs = sConfigMgr->GetOption<uint32>("DungeonClear.TestRun.OverallTimeoutS", 3600) * 1000;

    _record = DcTestRunRecord::Record{};
    _record.runId = MakeRunId();
    _record.dungeon = row->token;
    _record.dungeonName = row->name;
    _record.wing = row->wing;
    _record.mapId = row->mapId;
    _record.level = _level;
    _record.startedAtMs = NowUnixMs();
    _record.pauseGraceS = _limits.pauseGraceMs / 1000;
    _record.stallGraceS = _limits.stallGraceMs / 1000;
    _record.noProgressS = _limits.noProgressMs / 1000;
    _record.overallS = _limits.overallTimeoutMs / 1000;

    for (DcTestComp::Slot const& c : DcTestComp::kSlots)
    {
        Slot s;
        s.classId = c.classId;
        s.specName = c.specName;
        s.fallbackSpec = c.fallbackSpec;
        s.role = c.role;
        _slots.push_back(s);
    }

    // Pick one offline addclass-pool character per slot (the `addclass`
    // command's own selection rules) and start their async logins.
    PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(gm);
    bool const isAlliance = gm->GetTeamId(true) == TEAM_ALLIANCE;
    for (Slot& slot : _slots)
    {
        auto const& pool = sRandomPlayerbotMgr.addclassCache[
            RandomPlayerbotMgr::GetTeamClassIdx(isAlliance, slot.classId)];
        for (ObjectGuid const& guid : pool)
        {
            bool taken = false;
            for (Slot const& other : _slots)
                if (other.guid == guid)
                    taken = true;
            if (taken)
                continue;
            // (No botLoading check — it is protected on PlayerbotHolder;
            // AddPlayerBot itself no-ops on an in-flight load, and a char
            // loading for someone else just times the spawn stage out.)
            if (ObjectAccessor::FindConnectedPlayer(guid))
                continue;
            uint32 const guildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
            if (guildId && PlayerbotGuildMgr::instance().IsRealGuild(guildId))
                continue;
            slot.guid = guid;
            break;
        }
        if (!slot.guid)
        {
            std::string const why = std::string("no available ") + ClassToken(slot.classId) +
                                    " in the addclass pool — pre-seed with `.playerbots addclass`";
            if (err)
                *err = why;
            ResetState();
            return false;
        }
        mgr->AddPlayerBot(slot.guid, gm->GetSession()->GetAccountId());
    }

    LOG_INFO("playerbots.dungeonclear",
             "TESTRUN START {} dungeon={} map={} level={} gm={}",
             _record.runId, _record.dungeon, _mapId, _level, gm->GetName());

    EnterStage(Stage::SpawningBots);
    return true;
}

bool DcTestRunManager::Stop(std::string* msg)
{
    if (_stage == Stage::Idle)
    {
        if (msg)
            *msg = "no test run active";
        return false;
    }

    if (_stage == Stage::Monitoring)
    {
        {
            std::lock_guard<std::mutex> lock(_obsMutex);
            _abortRequested = true;
            _abortReason = "aborted by .dc test stop";
        }
        if (msg)
            *msg = "aborting test run " + _record.runId;
        return true;
    }

    // Mid-setup: fail out synchronously.
    if (msg)
        *msg = "aborting test run " + _record.runId + " during setup";
    FailSetup("aborted by .dc test stop");
    return true;
}

std::string DcTestRunManager::StatusText() const
{
    if (_stage == Stage::Idle)
        return "no test run active";

    std::string out = _record.runId + " " + _record.dungeon + " [" + StageName(_stage) +
                      "] elapsed " + std::to_string(_totalMs / 1000) + "s";
    if (_stage == Stage::Monitoring)
    {
        out += ", bosses " + std::to_string(_record.bossesKilled) + "/" +
               std::to_string(_record.bossesTotal);
        std::lock_guard<std::mutex> lock(_obsMutex);
        if (!_lastStatusState.empty())
            out += ", state " + _lastStatusState;
    }
    return out;
}

void DcTestRunManager::Tick(uint32 diff)
{
    if (_stage == Stage::Idle || _stage == Stage::TearingDown)
        return;

    _stageMs += diff;
    _totalMs += diff;

    // Live heartbeat for the dashboard's in-progress view. Overwritten every
    // ~2s; the dashboard treats a stale timestamp as "no run" so a crashed
    // worldserver can't leave a phantom live row.
    _liveAccumMs += diff;
    if (_liveAccumMs >= 2000)
    {
        _liveAccumMs = 0;
        WriteLiveStatus(true);
    }

    // The GM's session anchors the bots (their logout logs the party out);
    // without it the run cannot finish cleanly whatever stage it is in.
    if (!FindGm())
    {
        if (_stage == Stage::Monitoring)
            Finish(DcTestRun::Verdict::FailAborted, "GM logged out mid-run");
        else
            FailSetup("GM logged out during setup");
        return;
    }

    switch (_stage)
    {
        case Stage::SpawningBots:
            TickSpawning();
            break;
        case Stage::Provisioning:
            TickProvisioning();
            break;
        case Stage::Grouping:
            TickGrouping();
            break;
        case Stage::Teleporting:
            TickTeleporting();
            break;
        case Stage::Starting:
            TickStarting();
            break;
        case Stage::Monitoring:
            _monitorMs += diff;
            _monitorAccumMs += diff;
            if (_monitorAccumMs >= MONITOR_STEP_MS)
            {
                uint32 const dt = _monitorAccumMs;
                _monitorAccumMs = 0;
                TickMonitoring(dt);
            }
            break;
        default:
            break;
    }
}

void DcTestRunManager::TickSpawning()
{
    bool allIn = true;
    for (Slot const& slot : _slots)
    {
        Player* bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allIn = false;
            break;
        }
    }

    if (allIn)
    {
        EnterStage(Stage::Provisioning);
        return;
    }

    if (_stageMs >= SPAWN_TIMEOUT_MS)
        FailSetup("bots did not finish logging in (addclass pool empty, "
                  "maxAddedBots cap, or login failure — see server log)");
}

void DcTestRunManager::TickProvisioning()
{
    if (_stageMs >= PROVISION_TIMEOUT_MS)
    {
        FailSetup("provisioning timed out");
        return;
    }

    if (_provisionIdx >= _slots.size())
    {
        EnterStage(Stage::Grouping);
        return;
    }

    // One bot per tick — Randomize is heavyweight (full gear/spell/talent
    // roll) and five in one world tick would be a visible stall.
    Slot& slot = _slots[_provisionIdx];
    Player* bot = ObjectAccessor::FindPlayer(slot.guid);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !botAI)
        return;  // transient (mid world-add/teleport) — retry; stage timeout bounds it

    std::string pickedSpec;
    int const specNo = ResolveSpecNo(slot.classId, slot.specName, slot.fallbackSpec, &pickedSpec);
    if (specNo < 0 && (slot.role == std::string("tank") || slot.role == std::string("heal")))
    {
        // A random-rolled tank/healer spec would invalidate the whole run.
        FailSetup(std::string("no premade spec template matching '") + slot.specName +
                  "' for " + ClassToken(slot.classId) +
                  " (AiPlayerbot.PremadeSpecName.*) — cannot force the " + slot.role);
        return;
    }

    // Full roll at the target level first (Randomize includes GiveLevel and
    // re-picks talents), then force the role spec and re-gear for it — the
    // same sequence the `talents spec` chat command uses.
    PlayerbotFactory factory(bot, _level, ITEM_QUALITY_EPIC);
    factory.Randomize(false);
    if (specNo >= 0)
    {
        PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
        factory.InitEquipment(false, true);
        factory.InitGlyphs(false);
    }
    if (bot->getClass() == CLASS_HUNTER)
    {
        factory.InitPet();
        factory.InitAmmo();
    }
    botAI->ResetStrategies();

    DcTestRunRecord::CompEntry entry;
    entry.name = bot->GetName();
    entry.className = ClassToken(slot.classId);
    entry.spec = specNo >= 0 ? pickedSpec : "(random)";
    entry.role = slot.role;
    entry.guid = slot.guid.GetRawValue();
    entry.level = bot->GetLevel();
    _record.comp.push_back(entry);

    LOG_INFO("playerbots.dungeonclear", "TESTRUN {} provisioned {} ({} {}, level {})",
             _record.runId, bot->GetName(), entry.spec, entry.role, entry.level);

    slot.provisioned = true;
    ++_provisionIdx;
}

void DcTestRunManager::TickGrouping()
{
    if (!_groupFormed)
    {
        Player* tank = ObjectAccessor::FindPlayer(_slots[0].guid);
        if (!tank)
        {
            if (_stageMs >= GROUP_TIMEOUT_MS)
                FailSetup("tank vanished before grouping");
            return;
        }

        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                if (bot->GetGroup())
                    bot->RemoveFromGroup();

        // Direct group formation (the LFGMgr pattern) — no invite/accept
        // packet round-trips, and Create() makes the tank the leader.
        Group* group = new Group();
        if (!group->Create(tank))
        {
            delete group;
            FailSetup("group creation failed");
            return;
        }
        sGroupMgr->AddGroup(group);
        for (std::size_t i = 1; i < _slots.size(); ++i)
        {
            Player* bot = ObjectAccessor::FindPlayer(_slots[i].guid);
            if (!bot || !group->AddMember(bot))
            {
                FailSetup(std::string("could not add ") + ClassToken(_slots[i].classId) +
                          " to the group");
                return;
            }
        }
        group->SetDungeonDifficulty(DUNGEON_DIFFICULTY_NORMAL);
        _tankGuid = tank->GetGUID();
        _groupFormed = true;
    }

    Player* tank = FindTank();
    Group* group = tank ? tank->GetGroup() : nullptr;
    if (group && group->GetMembersCount() == _slots.size() && group->IsLeader(_tankGuid))
    {
        // Keep the GM (a real human Player) as each bot's playerbots MASTER, and
        // strip the stock follow-master strategy instead of nulling the master.
        //
        // Why not masterless: HasRealPlayerMaster() gates the whole "a human is
        // driving me" fast path in stock playerbots. With no real-player master,
        // GetReactDelay() returns base*10 (1000ms vs 100ms) out of combat, so the
        // party thinks on a 1s beat between packs — test runs looked far slower
        // and sloppier than a real human-led run. HasRealPlayerMaster() has no
        // map/distance/visibility gate (it only checks the master pointer is a
        // non-bot Player), so keeping the GM as master holds the fast path even
        // though the GM is invisible and outside the instance — the test now
        // mirrors a real run in every master-gated respect (react delay, AoE
        // avoidance, wait-for-attack), which is the point of a regression harness.
        //
        // The only reason the old code went masterless was to stop stock
        // follow-master from dragging the party toward the GM. Follow-master is
        // just the "follow" strategy, so we remove it outright here. DC's own
        // follow-tank redirect (a DcMovementAction, not a FollowAction) drives
        // the followers, and DungeonClearMultiplier additionally zeroes
        // FollowAction for every active-run member once the run is enabled —
        // this strip covers the pre-enable / paused window. A GM master sticks:
        // FindNewMaster() only re-resolves a null or bot master.
        Player* gm = FindGm();
        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    if (gm)
                        botAI->SetMaster(gm);
                    botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                    botAI->ChangeStrategy("-follow", BOT_STATE_COMBAT);
                }

        EnterStage(Stage::Teleporting);
        return;
    }

    if (_stageMs >= GROUP_TIMEOUT_MS)
        FailSetup("group did not form");
}

void DcTestRunManager::TickTeleporting()
{
    if (!_teleportIssued)
    {
        for (Slot const& slot : _slots)
        {
            Player* bot = ObjectAccessor::FindPlayer(slot.guid);
            if (!bot)
            {
                if (_stageMs >= TELEPORT_TIMEOUT_MS)
                    FailSetup("bot vanished before teleport");
                return;  // transient — retry next tick
            }
        }
        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                bot->TeleportTo(_mapId, _x, _y, _z, _o);
        _teleportIssued = true;
    }

    bool allThere = true;
    for (Slot const& slot : _slots)
    {
        Player* bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || bot->GetMapId() != _mapId || !bot->IsInWorld() || bot->IsBeingTeleported())
        {
            allThere = false;
            break;
        }
    }

    if (allThere)
    {
        EnterStage(Stage::Starting);
        return;
    }

    if (_stageMs >= TELEPORT_TIMEOUT_MS)
        FailSetup("party did not arrive at the dungeon entrance");
}

void DcTestRunManager::TickStarting()
{
    Player* tank = FindTank();
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    if (!tank || !tankAI)
    {
        if (_stageMs >= START_TIMEOUT_MS)
            FailSetup("tank vanished before start");
        return;
    }

    // Freshly-teleported bots may not have the DC strategies installed yet
    // (contexts register on world ticks) — re-assert every tick; idempotent.
    for (Slot const& slot : _slots)
        if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
            DcStrategyGate::Reconcile(bot);

    AiObjectContext* ctx = tankAI->GetAiObjectContext();

    // A reused instance with dead bosses would flash an instant (false)
    // all-clear — refuse it rather than record a fake success.
    if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
        if (inst->GetCompletedEncounterMask() != 0)
        {
            FailSetup("stale instance: encounters already completed (mask " +
                      std::to_string(inst->GetCompletedEncounterMask()) + ")");
            return;
        }

    // The boss roster needs a tick or two to populate after the teleport;
    // retry inside the stage timeout.
    std::vector<DungeonBossInfo> const bosses =
        ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get();
    if (bosses.empty())
    {
        if (_stageMs >= START_TIMEOUT_MS)
            FailSetup("no boss roster for this map");
        return;
    }

    if (!_dcOnIssued)
    {
        _record.instanceId = tank->GetMap()->GetInstanceId();
        _roster.clear();
        for (DungeonBossInfo const& b : bosses)
        {
            BossRef ref;
            ref.entry = b.entry;
            ref.encounterIndex = b.encounterIndex;
            ref.name = b.name;
            ref.isBoss = b.kind == DungeonAnchorKind::Boss;
            _roster.push_back(ref);
        }
        _record.bossesTotal = static_cast<uint32>(_roster.size());

        // The run must never wait for a human: kill the WaitAtBoss pre-pull
        // hold for this run whatever the conf says.
        DcSettings::SetOverride(_tankGuid, "WaitAtBoss", 0.0);
        _dcOnIssued = true;
    }

    // Retry `dc on` each tick until the enabled flag sticks (roster/context
    // timing) or the stage times out.
    tankAI->DoSpecificAction("dc on", Event("dc", "", FindGm()), true);
    if (DcRun::Of(ctx).enabled)
    {
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
            _lastMask = inst->GetCompletedEncounterMask();
        _lastAnchors =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();
        LOG_INFO("playerbots.dungeonclear",
                 "TESTRUN {} running: instance {} with {} bosses/objectives",
                 _record.runId, _record.instanceId, _record.bossesTotal);
        EnterStage(Stage::Monitoring);
        return;
    }

    if (_stageMs >= START_TIMEOUT_MS)
        FailSetup("dc on did not take (see server log for the bot's error chat)");
}

void DcTestRunManager::TickMonitoring(uint32 dt)
{
    DcTestRun::Observation obs;
    obs.elapsedMs = _monitorMs;
    obs.gmOnline = true;  // GM absence is handled in Tick()

    {
        std::lock_guard<std::mutex> lock(_obsMutex);
        obs.abortRequested = _abortRequested;
        obs.disableFired = _disableFired;
        obs.disableAllCleared = _disableAllCleared;
    }

    // Connected-lookup + explicit teleport tolerance: DC's own scripted
    // events teleport the party mid-run (Underbog hops, Old Hillsbrad's
    // muster) — a sample landing in that window must wait, not read the
    // leader as gone.
    Player* tank = ObjectAccessor::FindConnectedPlayer(_tankGuid);
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    std::string pauseReason;
    std::string stallReason;

    if (tank && tankAI && (!tank->IsInWorld() || tank->IsBeingTeleported()))
        return;  // mid-teleport — skip this sample, timers resume next tick

    if (!tank || !tankAI)
    {
        obs.leaderMissing = true;
    }
    else
    {
        AiObjectContext* ctx = tankAI->GetAiObjectContext();
        DcRunState const& rs = DcRun::Of(ctx);

        // The disable funnel notifies OnRunDisabled; this catches any path
        // that somehow bypassed it (belt and braces — enabled off without a
        // callback still ends the run).
        if (!obs.disableFired && !rs.enabled)
        {
            obs.disableFired = true;
            std::lock_guard<std::mutex> lock(_obsMutex);
            if (_disableReason.empty())
                _disableReason = "run disabled (no reason captured)";
        }

        // Progress = a new completed-encounter bit or a new cleared anchor.
        uint32 mask = _lastMask;
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
            mask = inst->GetCompletedEncounterMask();
        std::size_t const anchors =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();

        bool progressed = false;
        if (uint32 const fresh = mask & ~_lastMask)
        {
            for (uint32 bit = 0; bit < 32; ++bit)
            {
                if (!(fresh & (1u << bit)))
                    continue;
                DcTestRunRecord::BossKill kill;
                kill.t = _totalMs / 1000;
                kill.via = "mask";
                for (BossRef const& ref : _roster)
                    if (ref.encounterIndex == bit)
                    {
                        kill.entry = ref.entry;
                        kill.name = ref.name;
                        break;
                    }
                if (kill.name.empty())
                    kill.name = "encounter #" + std::to_string(bit);
                _record.bossTimeline.push_back(kill);
                progressed = true;
            }
            _lastMask = mask;
        }
        if (anchors > _lastAnchors)
        {
            for (std::size_t i = _lastAnchors; i < anchors; ++i)
            {
                DcTestRunRecord::BossKill kill;
                kill.t = _totalMs / 1000;
                kill.name = "objective";
                kill.via = "anchor";
                _record.bossTimeline.push_back(kill);
            }
            _lastAnchors = anchors;
            progressed = true;
        }
        if (progressed)
        {
            _record.bossesKilled =
                std::min<uint32>(static_cast<uint32>(_record.bossTimeline.size()),
                                 _record.bossesTotal);
            _sinceProgressMs = 0;
        }
        else
            _sinceProgressMs += dt;

        // Pause / stall trackers.
        if (rs.paused)
        {
            pauseReason = rs.pauseReason;
            if (!_wasPaused)
            {
                _record.pauses.push_back({_totalMs / 1000, pauseReason});
                _pausedForMs = 0;
            }
            else
                _pausedForMs += dt;
            _wasPaused = true;
        }
        else
        {
            _wasPaused = false;
            _pausedForMs = 0;
        }
        obs.paused = rs.paused;
        obs.pausedForMs = _pausedForMs;

        stallReason = ctx->GetValue<std::string&>(DcKey::StallReason)->Get();
        if (!stallReason.empty())
            _stalledForMs += dt;
        else
            _stalledForMs = 0;
        obs.stalled = !stallReason.empty();
        obs.stalledForMs = _stalledForMs;

        obs.sinceProgressMs = _sinceProgressMs;
    }

    DcTestRun::Verdict const verdict = DcTestRun::Classify(obs, _limits);
    if (!DcTestRun::IsTerminal(verdict))
        return;

    std::string failReason;
    switch (verdict)
    {
        case DcTestRun::Verdict::Success:
            break;
        case DcTestRun::Verdict::FailDisabled:
        {
            std::lock_guard<std::mutex> lock(_obsMutex);
            failReason = "run disabled: " + _disableReason;
            break;
        }
        case DcTestRun::Verdict::FailPausedTimeout:
            failReason = "paused for over " + std::to_string(_limits.pauseGraceMs / 1000) +
                         "s: " + (pauseReason.empty() ? "(no reason)" : pauseReason);
            break;
        case DcTestRun::Verdict::FailStalledTimeout:
            failReason = "stalled for over " + std::to_string(_limits.stallGraceMs / 1000) +
                         "s: " + (stallReason.empty() ? "(no reason)" : stallReason);
            break;
        case DcTestRun::Verdict::FailNoProgress:
            failReason = "no boss/objective progress for " +
                         std::to_string(_limits.noProgressMs / 1000) + "s";
            break;
        case DcTestRun::Verdict::FailOverallTimeout:
            failReason = "exceeded the overall time limit (" +
                         std::to_string(_limits.overallTimeoutMs / 1000) + "s)";
            break;
        case DcTestRun::Verdict::FailAborted:
        {
            std::lock_guard<std::mutex> lock(_obsMutex);
            failReason = obs.leaderMissing ? "leader tank vanished"
                         : (_abortReason.empty() ? "aborted" : _abortReason);
            break;
        }
        default:
            break;
    }

    Finish(verdict, failReason);
}

void DcTestRunManager::FailSetup(std::string const& why)
{
    _record.setupStage = StageName(_stage);
    _record.result = "setup_failed";
    _record.failReason = why;
    LOG_INFO("playerbots.dungeonclear", "TESTRUN {} setup failed at {}: {}",
             _record.runId, _record.setupStage, why);
    Teardown();
}

void DcTestRunManager::Finish(DcTestRun::Verdict verdict, std::string const& failReason)
{
    _record.result = DcTestRun::VerdictName(verdict);
    _record.failReason = failReason;
    {
        std::lock_guard<std::mutex> lock(_obsMutex);
        _record.disableReason = _disableReason;
    }
    Teardown();
}

void DcTestRunManager::Teardown()
{
    _stage = Stage::TearingDown;  // gates the observers off before DisableDungeonClear

    Player* tank = FindTank();
    if (tank)
    {
        _record.finalMap = tank->GetMapId();
        _record.finalX = tank->GetPositionX();
        _record.finalY = tank->GetPositionY();
        _record.finalZ = tank->GetPositionZ();

        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
            if (DcRun::Of(tankAI).enabled)
                DcActionShared::DisableDungeonClear(tankAI, "test run teardown");
    }

    if (_tankGuid)
        DcSettings::ClearRun(_tankGuid);

    if (tank)
        if (Group* group = tank->GetGroup())
            group->Disband(true);

    Player* gm = FindGm();
    PlayerbotMgr* mgr = gm ? GET_PLAYERBOT_MGR(gm) : nullptr;
    if (mgr)
        for (Slot const& slot : _slots)
            if (slot.guid)
                mgr->LogoutPlayerBot(slot.guid);

    _record.endedAtMs = NowUnixMs();
    _record.durationS = static_cast<uint32>((_record.endedAtMs - _record.startedAtMs) / 1000);
    DcTestRunRecord::Append(_record);
    WriteLiveStatus(false);

    LOG_INFO("playerbots.dungeonclear", "TESTRUN END {} result={} reason={} bosses={}/{} duration={}s",
             _record.runId, _record.result, _record.failReason,
             _record.bossesKilled, _record.bossesTotal, _record.durationS);

    if (gm)
        ChatHandler(gm->GetSession()).SendSysMessage(Acore::StringFormat(
            "Test run {}: {} ({}/{} bosses, {}s){}", _record.dungeon, _record.result,
            _record.bossesKilled, _record.bossesTotal, _record.durationS,
            _record.failReason.empty() ? "" : (" — " + _record.failReason)));

    ResetState();
}

void DcTestRunManager::ResetState()
{
    _stage = Stage::Idle;
    _dungeonToken.clear();
    _mapId = 0;
    _x = _y = _z = _o = 0.f;
    _level = 0;
    _gmGuid.Clear();
    _tankGuid.Clear();
    _slots.clear();
    _limits = DcTestRun::Limits{};
    _stageMs = 0;
    _totalMs = 0;
    _monitorMs = 0;
    _monitorAccumMs = 0;
    _provisionIdx = 0;
    _groupFormed = false;
    _teleportIssued = false;
    _dcOnIssued = false;
    _roster.clear();
    _lastMask = 0;
    _lastAnchors = 0;
    _sinceProgressMs = 0;
    _pausedForMs = 0;
    _stalledForMs = 0;
    _wasPaused = false;

    std::lock_guard<std::mutex> lock(_obsMutex);
    _abortRequested = false;
    _abortReason.clear();
    _disableFired = false;
    _disableAllCleared = false;
    _disableReason.clear();
    _lastStatusState.clear();
}

void DcTestRunManager::WriteLiveStatus(bool active)
{
    std::ofstream f(DcTestRunRecord::LivePath(), std::ios::out | std::ios::trunc);
    if (!f.is_open())
        return;

    using DcTestRunRecord::EscapeJson;
    std::ostringstream s;
    s << "{\"active\":" << (active ? "true" : "false")
      << ",\"ts\":" << (NowUnixMs() / 1000);
    if (active)
    {
        s << ",\"runId\":\"" << EscapeJson(_record.runId) << '"'
          << ",\"dungeon\":\"" << EscapeJson(_record.dungeon) << '"'
          << ",\"dungeonName\":\"" << EscapeJson(_record.dungeonName) << '"'
          << ",\"stage\":\"" << StageName(_stage) << '"'
          << ",\"level\":" << _level
          << ",\"elapsedS\":" << _totalMs / 1000
          << ",\"bossesKilled\":" << _record.bossesKilled
          << ",\"bossesTotal\":" << _record.bossesTotal;

        std::lock_guard<std::mutex> lock(_obsMutex);
        s << ",\"state\":\"" << EscapeJson(_lastStatusState) << '"';
        s << ",\"recent\":[";
        std::vector<DcTestRunRecord::StatusEntry> const& st = _record.statusTimeline;
        std::size_t const from = st.size() > 8 ? st.size() - 8 : 0;
        for (std::size_t i = from; i < st.size(); ++i)
        {
            if (i != from)
                s << ',';
            s << "{\"t\":" << st[i].t << ",\"state\":\"" << EscapeJson(st[i].state)
              << "\",\"detail\":\"" << EscapeJson(st[i].detail) << "\"}";
        }
        s << ']';
    }
    s << '}';
    f << s.str() << '\n';
}

void DcTestRunManager::OnRunDisabled(Player* leader, std::string const& reason)
{
    if (_stage != Stage::Monitoring || !leader || leader->GetGUID() != _tankGuid)
        return;

    std::lock_guard<std::mutex> lock(_obsMutex);
    if (_disableFired)
        return;  // first reason wins
    _disableFired = true;
    _disableReason = reason;
    _disableAllCleared = reason == DcActionShared::kReasonAllCleared;
}

void DcTestRunManager::OnStatusPayload(ObjectGuid tank, std::string const& payload)
{
    if ((_stage != Stage::Monitoring && _stage != Stage::Starting) || tank != _tankGuid)
        return;

    // Payload: STATUS \t enabled \t bossEntry \t bossName \t stall \t skipped
    //          \t state \t detail \t pullSetting \t pullDecision
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
    if (parts.size() < 8 || parts[0] != "STATUS")
        return;

    std::lock_guard<std::mutex> lock(_obsMutex);
    if (parts[6] == _lastStatusState)
        return;
    _lastStatusState = parts[6];
    _record.statusTimeline.push_back({_totalMs / 1000, parts[6], parts[7]});
}
