/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARCHATSTRATEGY_H
#define _PLAYERBOT_DUNGEONCLEARCHATSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// Sibling strategy that holds only the chat-keyword TriggerNodes for
// `dc on/off/skip/status/bosses` (plus the long aliases). Registered on the
// combat, non-combat, and dead engines so chat commands are processed
// regardless of the bot's current engine state — without this, `dc off`
// issued while the tank is mid-pull is silently queued on the shared
// ChatCommandTrigger until non-combat ticks again.
class DungeonClearChatStrategy : public Strategy
{
public:
    DungeonClearChatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "dungeon clear chat"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
