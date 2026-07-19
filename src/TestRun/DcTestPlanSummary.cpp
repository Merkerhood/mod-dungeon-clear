/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestPlanSummary.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "TestRun/DcTestRunRecord.h"

namespace DcTestPlanSummary
{
    namespace
    {
        // Ordered accumulation (std::map keyed by string) then a stable sort by
        // count desc — ties stay alphabetical, so output order is deterministic.
        std::vector<KeyCount> CountSorted(std::map<std::string, std::uint32_t> const& counts)
        {
            std::vector<KeyCount> out;
            out.reserve(counts.size());
            for (auto const& [key, count] : counts)
                out.push_back({key, count});
            std::stable_sort(out.begin(), out.end(),
                             [](KeyCount const& a, KeyCount const& b) { return a.count > b.count; });
            return out;
        }
    }

    Stats Build(std::vector<DcTestPlan::RunOutcome> const& outcomes)
    {
        Stats s;
        s.launched = static_cast<std::uint32_t>(outcomes.size());

        std::map<std::string, std::uint32_t> verdicts;
        std::map<std::string, std::uint32_t> reasons;
        std::vector<std::uint32_t> successDurations;

        // Funnel: per boss name, kill count (deduped within a run) and the sum
        // of timeline positions, so entries sort into progression order by
        // mean position even when runs kill in slightly different orders.
        struct FunnelAcc
        {
            std::uint32_t killed = 0;
            std::uint64_t posSum = 0;
        };
        std::map<std::string, FunnelAcc> funnel;

        for (DcTestPlan::RunOutcome const& o : outcomes)
        {
            s.runIds.push_back(o.runId);
            ++verdicts[o.result];
            if (o.result == "success")
            {
                ++s.succeeded;
                successDurations.push_back(o.durationS);
            }
            else
            {
                ++s.failed;
                if (!o.failReason.empty())
                    ++reasons[o.failReason];
            }

            std::vector<std::string> seen;
            for (std::size_t pos = 0; pos < o.bossKills.size(); ++pos)
            {
                std::string const& name = o.bossKills[pos];
                if (std::find(seen.begin(), seen.end(), name) != seen.end())
                    continue;
                seen.push_back(name);
                FunnelAcc& acc = funnel[name];
                ++acc.killed;
                acc.posSum += pos;
            }
        }

        s.verdicts = CountSorted(verdicts);
        s.failReasons = CountSorted(reasons);

        if (!successDurations.empty())
        {
            std::sort(successDurations.begin(), successDurations.end());
            s.minS = successDurations.front();
            s.maxS = successDurations.back();
            std::uint64_t sum = 0;
            for (std::uint32_t d : successDurations)
                sum += d;
            s.avgS = static_cast<std::uint32_t>(sum / successDurations.size());
            std::size_t const n = successDurations.size();
            s.medianS = n % 2 ? successDurations[n / 2]
                              : (successDurations[n / 2 - 1] + successDurations[n / 2]) / 2;
        }

        std::vector<std::pair<double, std::string>> ordered;
        for (auto const& [name, acc] : funnel)
            ordered.emplace_back(static_cast<double>(acc.posSum) / acc.killed, name);
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [pos, name] : ordered)
            s.funnel.push_back({name, funnel[name].killed});

        return s;
    }

    std::string ToJsonl(Header const& h, Stats const& s)
    {
        using DcTestRunRecord::EscapeJson;

        auto str = [](std::string const& v) { return "\"" + EscapeJson(v) + "\""; };

        std::ostringstream o;
        o << "{\"schema\":" << h.schema
          << ",\"planId\":" << str(h.planId)
          << ",\"dungeon\":" << str(h.dungeon)
          << ",\"dungeonName\":" << str(h.dungeonName)
          << ",\"requested\":{\"total\":" << h.total
          << ",\"concurrent\":" << h.concurrent
          << ",\"level\":" << h.level
          << ",\"seedBase\":" << h.seedBase
          << "},\"startedAtMs\":" << h.startedAtMs
          << ",\"endedAtMs\":" << h.endedAtMs
          << ",\"durationS\":" << h.durationS
          << ",\"result\":" << str(h.result)
          << ",\"abortReason\":" << str(h.abortReason)
          << ",\"runs\":{\"launched\":" << s.launched
          << ",\"succeeded\":" << s.succeeded
          << ",\"failed\":" << s.failed
          << "},\"verdicts\":{";
        for (std::size_t i = 0; i < s.verdicts.size(); ++i)
        {
            if (i)
                o << ',';
            o << str(s.verdicts[i].key) << ':' << s.verdicts[i].count;
        }
        o << "},\"failReasons\":[";
        for (std::size_t i = 0; i < s.failReasons.size(); ++i)
        {
            if (i)
                o << ',';
            o << "{\"reason\":" << str(s.failReasons[i].key)
              << ",\"count\":" << s.failReasons[i].count << '}';
        }
        o << "],\"duration\":{\"minS\":" << s.minS
          << ",\"avgS\":" << s.avgS
          << ",\"medianS\":" << s.medianS
          << ",\"maxS\":" << s.maxS
          << "},\"bossFunnel\":[";
        for (std::size_t i = 0; i < s.funnel.size(); ++i)
        {
            if (i)
                o << ',';
            o << "{\"name\":" << str(s.funnel[i].name)
              << ",\"killed\":" << s.funnel[i].killed << '}';
        }
        o << "],\"runIds\":[";
        for (std::size_t i = 0; i < s.runIds.size(); ++i)
        {
            if (i)
                o << ',';
            o << str(s.runIds[i]);
        }
        o << "]}";
        return o.str();
    }

    std::string CapturePath()
    {
        if (char const* env = std::getenv("DC_TESTPLANS_FILE"))
            if (env[0])
                return env;
        return "dc_testplans.jsonl";
    }

    void Append(Header const& h, Stats const& s)
    {
        static std::mutex mtx;
        static std::ofstream file;
        static bool opened = false;

        std::lock_guard<std::mutex> lock(mtx);
        if (!opened)
        {
            file.open(CapturePath(), std::ios::out | std::ios::app);
            opened = true;
        }
        if (!file.is_open())
            return;

        file << ToJsonl(h, s) << '\n';
        file.flush();
    }
}
