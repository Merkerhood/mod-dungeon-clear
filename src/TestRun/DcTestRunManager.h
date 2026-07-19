/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTRUNMANAGER_H
#define _PLAYERBOT_DCTESTRUNMANAGER_H

#include <mutex>
#include <string>
#include <vector>

#include "ObjectGuid.h"
#include "TestRun/DcTestRunRecord.h"
#include "TestRun/DcTestRunVerdict.h"

class Player;

// The `.dc test` harness: one command spawns a full 5-bot party (prot warrior
// tank, holy priest, three DPS — DcTestComp), levels/specs/gears it, groups it
// under the tank, teleports it to a supported dungeon's entrance
// (DcTestDungeonRegistry) and starts an autonomous dungeon-clear run. The GM
// who issued the command stays outside the party; the bots are added under the
// GM's account (so they are exempt from the random-bot rotation) and the run
// is driven entirely by the existing dungeon-clear leader machinery.
//
// One run at a time, driven as an explicit state machine off the global
// playerbots tick:
//
//   Idle -> SpawningBots -> Provisioning -> Grouping -> Teleporting
//        -> Starting -> Monitoring -> TearingDown -> (JSONL record) -> Idle
//
// Success/failure is decided by the pure kernel in DcTestRunVerdict.h fed
// from two observers plus per-tick reads of the leader's run state:
//   * DcActionShared::DisableDungeonClear notifies OnRunDisabled — the run
//     ending on its own (all-cleared = success; death/off/left = failure).
//   * DcStatusPublisher's change-detector notifies OnStatusPayload — the
//     status-transition timeline for the record.
// Every terminal outcome appends one line to dc_testruns.jsonl
// (DcTestRunRecord) and logs TESTRUN START/END markers on
// playerbots.dungeonclear so the DungeonClear.log slice is greppable.
//
// All cross-tick bookkeeping is GUIDs + POD (never a stored Player*); the
// observers may fire from map-update threads, so the fields they write sit
// behind a small mutex.
class DcTestRunManager
{
public:
    static DcTestRunManager& Instance();

    // Validates and launches a run. levelOverride 0 = the registry row's
    // recommended level. False + err on: run already active, unknown dungeon,
    // no playerbot manager on the GM, empty addclass pool for a comp class.
    bool Start(Player* gm, std::string const& dungeonToken, uint32 levelOverride,
               std::string* err);

    // Aborts the active run (recorded as fail/aborted). False when idle.
    bool Stop(std::string* msg);

    std::string StatusText() const;
    bool IsActive() const { return _stage != Stage::Idle; }

    // Drive from the global playerbots tick (world thread).
    void Tick(uint32 diff);

    // Observer: every DisableDungeonClear lands here; ignored unless it is
    // this run's leader ending a monitored run.
    void OnRunDisabled(Player* leader, std::string const& reason);

    // Observer: DcStatusPublisher pushes each changed STATUS payload here.
    void OnStatusPayload(ObjectGuid tank, std::string const& payload);

private:
    enum class Stage : uint8
    {
        Idle,
        SpawningBots,
        Provisioning,
        Grouping,
        Teleporting,
        Starting,
        Monitoring,
        TearingDown
    };

    struct Slot
    {
        uint8 classId = 0;
        char const* specName = "";
        char const* fallbackSpec = "";
        char const* role = "";
        ObjectGuid guid;
        bool provisioned = false;
    };

    // Boss-roster snapshot taken at Starting so mask deltas can be named
    // without touching live values from the monitor loop.
    struct BossRef
    {
        uint32 entry = 0;
        uint32 encounterIndex = 0;
        std::string name;
        bool isBoss = true;
    };

    DcTestRunManager() = default;

    void EnterStage(Stage s);
    static char const* StageName(Stage s);

    void TickSpawning();
    void TickProvisioning();
    void TickGrouping();
    void TickTeleporting();
    void TickStarting();
    void TickMonitoring(uint32 dt);

    // Setup-stage failure: records result="setup_failed" with the stage tag.
    void FailSetup(std::string const& why);
    // Terminal verdict from monitoring.
    void Finish(DcTestRun::Verdict verdict, std::string const& failReason);
    // Disable run, drop overrides, disband, log bots out, append the record.
    void Teardown();
    void ResetState();

    // Overwrite the dc_testrun_live.json heartbeat the dashboard polls.
    void WriteLiveStatus(bool active);

    Player* FindGm() const;
    Player* FindTank() const;

    // --- run identity (set in Start) ---------------------------------------
    Stage _stage = Stage::Idle;
    std::string _dungeonToken;
    uint32 _mapId = 0;
    float _x = 0.f, _y = 0.f, _z = 0.f, _o = 0.f;
    uint32 _level = 0;
    ObjectGuid _gmGuid;
    ObjectGuid _tankGuid;
    std::vector<Slot> _slots;
    DcTestRunRecord::Record _record;
    DcTestRun::Limits _limits;

    // --- stage bookkeeping --------------------------------------------------
    uint32 _stageMs = 0;      // time in current stage (stage timeouts)
    uint32 _totalMs = 0;      // time since Start (timeline t offsets)
    uint32 _liveAccumMs = 0;  // heartbeat-file throttle
    uint32 _monitorMs = 0;    // time in Monitoring (kernel elapsed)
    uint32 _monitorAccumMs = 0;
    std::size_t _provisionIdx = 0;
    bool _groupFormed = false;
    bool _teleportIssued = false;
    bool _dcOnIssued = false;

    // --- monitoring state ----------------------------------------------------
    std::vector<BossRef> _roster;
    uint32 _lastMask = 0;
    std::size_t _lastAnchors = 0;
    uint32 _sinceProgressMs = 0;
    uint32 _pausedForMs = 0;
    uint32 _stalledForMs = 0;
    bool _wasPaused = false;

    // --- observer-written (any thread) --------------------------------------
    mutable std::mutex _obsMutex;
    bool _abortRequested = false;
    std::string _abortReason;
    bool _disableFired = false;
    bool _disableAllCleared = false;
    std::string _disableReason;
    std::string _lastStatusState;
};

#endif  // _PLAYERBOT_DCTESTRUNMANAGER_H
