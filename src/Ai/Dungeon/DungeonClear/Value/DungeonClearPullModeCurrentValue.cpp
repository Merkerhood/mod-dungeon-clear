/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearPullModeCurrentValue.h"

#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"

bool DungeonClearPullModeCurrentValue::Calculate()
{
    // Refresh the Dynamic (pull setting == 2) verdict for THIS tick, then report
    // the behavioural bool. UpdateDynamicPullMode is a no-op for Off/On (where
    // DcPullAction owns the bool) and internally throttles the expensive
    // classification, so running it on every read is cheap and idempotent.
    DcPullPlanner::UpdateDynamicPullMode(botAI, context);
    return AI_VALUE(bool, "dungeon clear pull mode");
}
