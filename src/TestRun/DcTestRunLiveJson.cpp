/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestRunLiveJson.h"

#include <sstream>

#include "TestRun/DcTestRunRecord.h"

namespace DcTestRunLive
{
    std::string Build(std::uint64_t tsS, std::vector<RunSnapshot> const& runs)
    {
        using DcTestRunRecord::EscapeJson;

        std::ostringstream s;
        s << "{\"active\":" << (runs.empty() ? "false" : "true")
          << ",\"ts\":" << tsS
          << ",\"runs\":[";
        for (std::size_t r = 0; r < runs.size(); ++r)
        {
            RunSnapshot const& run = runs[r];
            if (r)
                s << ',';
            s << "{\"runId\":\"" << EscapeJson(run.runId) << '"'
              << ",\"dungeon\":\"" << EscapeJson(run.dungeon) << '"'
              << ",\"dungeonName\":\"" << EscapeJson(run.dungeonName) << '"'
              << ",\"stage\":\"" << EscapeJson(run.stage) << '"'
              << ",\"level\":" << run.level
              << ",\"elapsedS\":" << run.elapsedS
              << ",\"bossesKilled\":" << run.bossesKilled
              << ",\"bossesTotal\":" << run.bossesTotal
              << ",\"state\":\"" << EscapeJson(run.state) << '"'
              << ",\"recent\":[";
            for (std::size_t i = 0; i < run.recent.size(); ++i)
            {
                StatusEntry const& e = run.recent[i];
                if (i)
                    s << ',';
                s << "{\"t\":" << e.t << ",\"state\":\"" << EscapeJson(e.state)
                  << "\",\"detail\":\"" << EscapeJson(e.detail) << "\"}";
            }
            s << "]}";
        }
        s << "]}";
        return s.str();
    }
}
