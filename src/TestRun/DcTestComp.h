/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTCOMP_H
#define _PLAYERBOT_DCTESTCOMP_H

#include <cstddef>
#include <cstdint>

// The fixed party composition a test run fields: prot warrior tank, holy
// priest healer, three DPS. Specs are named by the playerbots premade-spec
// template name (AiPlayerbot.PremadeSpecName.<class>.<n>) and resolved to a
// specNo at provisioning time, so a reordered conf can't silently flip the
// tank into arms. Death knights are excluded (level floor breaks low-level
// dungeons); all-Alliance because the addclass pool is picked per team and
// entrances are faction-agnostic teleports. v1 is deliberately one comp —
// deterministic runs make regressions comparable across dungeons and days.

namespace DcTestComp
{
    struct Slot
    {
        std::uint8_t classId;      // 1=warrior 3=hunter 4=rogue 5=priest 8=mage
        char const* specName;      // premade-spec template to force
        char const* fallbackSpec;  // substring to match if the exact name is absent
        char const* role;          // "tank" | "heal" | "dps" (for the record/UI)
    };

    inline constexpr Slot kSlots[] = {
        { 1, "prot pve",   "prot",   "tank" },
        { 5, "holy pve",   "holy",   "heal" },
        { 8, "frost pve",  "frost",  "dps"  },
        { 4, "combat pve", "combat", "dps"  },
        { 3, "mm pve",     "mm",     "dps"  },
    };

    inline constexpr std::size_t kPartySize = sizeof(kSlots) / sizeof(kSlots[0]);
}

#endif  // _PLAYERBOT_DCTESTCOMP_H
