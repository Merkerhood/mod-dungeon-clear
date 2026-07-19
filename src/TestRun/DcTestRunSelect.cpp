/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestRunSelect.h"

namespace DcTestRunSelect
{
    Result Resolve(std::string const& selector, std::vector<RunRef> const& runs)
    {
        if (runs.empty())
            return {Kind::NoRuns, {}};

        // 1. Bare selector — the one run, else ambiguous.
        if (selector.empty())
        {
            if (runs.size() == 1)
                return {Kind::All, {0}};
            return {Kind::Ambiguous, {}};
        }

        // 2. "all" — everything.
        if (selector == "all")
        {
            std::vector<std::size_t> all;
            all.reserve(runs.size());
            for (std::size_t i = 0; i < runs.size(); ++i)
                all.push_back(i);
            return {Kind::All, all};
        }

        // 3. Exact runId — wins over a same-string dungeon token.
        for (std::size_t i = 0; i < runs.size(); ++i)
            if (runs[i].runId == selector)
                return {Kind::Matched, {i}};

        // 4. Dungeon token — every run of that dungeon.
        std::vector<std::size_t> byDungeon;
        for (std::size_t i = 0; i < runs.size(); ++i)
            if (runs[i].dungeon == selector)
                byDungeon.push_back(i);
        if (!byDungeon.empty())
            return {Kind::Matched, byDungeon};

        // 5. Nothing matched.
        return {Kind::NotFound, {}};
    }
}
