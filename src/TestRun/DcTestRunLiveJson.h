/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTRUNLIVEJSON_H
#define _PLAYERBOT_DCTESTRUNLIVEJSON_H

#include <cstdint>
#include <string>
#include <vector>

// Pure builder for the live-heartbeat sidecar the dashboard polls
// (dc_testrun_live.json). With concurrent runs the file carries an array:
//
//   {"active":<bool>,"ts":<unixS>,"runs":[ {…per-run keys…}, … ]}
//
// "active" is true whenever at least one run is present; the dashboard treats a
// stale ts (or active:false / empty runs) as "no live run". Each per-run object
// keeps the same keys the single-run schema wrote, so app.js's card renderer
// maps 1:1 over the array. Engine-free (takes plain snapshots the manager
// gathers under its own locks) so the schema is unit-testable in isolation.

namespace DcTestRunLive
{
    struct StatusEntry
    {
        std::uint32_t t = 0;
        std::string state;
        std::string detail;
    };

    // One party member's live position for the dashboard map overlay. Kept as a
    // compact numeric record (short keys, 1-decimal coords) and emitted in its
    // own "bots" array — deliberately NOT folded into the human-readable
    // "recent" timeline, so streaming per-tick coordinates never floods the
    // status lines the dashboard/logs render as text.
    struct BotPos
    {
        std::string role;              // "tank" / "healer" / "dps"
        std::uint8_t classId = 0;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool alive = true;
    };

    struct RunSnapshot
    {
        std::string runId;
        std::string dungeon;
        std::string dungeonName;
        std::string stage;
        std::string state;             // last publisher state token
        std::uint32_t level = 0;
        std::uint32_t elapsedS = 0;
        std::uint32_t bossesKilled = 0;
        std::uint32_t bossesTotal = 0;
        std::int32_t mapId = -1;           // party's current map; -1 = none resolved
        std::vector<BotPos> bots;          // per-member positions (may be empty)
        std::vector<StatusEntry> recent;   // already trimmed to <=8 by the caller
    };

    // tsS = unix seconds stamped by the caller (engine time source stays out of
    // the pure builder). runs empty → {"active":false,"ts":tsS,"runs":[]}.
    std::string Build(std::uint64_t tsS, std::vector<RunSnapshot> const& runs);
}

#endif  // _PLAYERBOT_DCTESTRUNLIVEJSON_H
