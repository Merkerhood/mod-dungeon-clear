/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARSTRATEGYCONTEXT_H
#define _PLAYERBOT_DUNGEONCLEARSTRATEGYCONTEXT_H

#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DungeonClearChatStrategy.h"
#include "Ai/Dungeon/DungeonClear/Strategy/DungeonClearStrategy.h"

class DungeonClearStrategyContext : public NamedObjectContext<Strategy>
{
public:
    DungeonClearStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["dungeon clear"] = &DungeonClearStrategyContext::dungeon_clear;
        creators["dungeon clear chat"] = &DungeonClearStrategyContext::dungeon_clear_chat;
    }

private:
    static Strategy* dungeon_clear(PlayerbotAI* ai) { return new DungeonClearStrategy(ai); }
    static Strategy* dungeon_clear_chat(PlayerbotAI* ai) { return new DungeonClearChatStrategy(ai); }
};

#endif
