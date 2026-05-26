/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearChatStrategy.h"

#include "Playerbots.h"

void DungeonClearChatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    constexpr float chatRel = 100.0f;
    triggers.push_back(new TriggerNode("dc on",             { NextAction("dc on",     chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear on",  { NextAction("dc on",     chatRel) }));
    triggers.push_back(new TriggerNode("dc off",            { NextAction("dc off",    chatRel) }));
    triggers.push_back(new TriggerNode("dungeon clear off", { NextAction("dc off",    chatRel) }));
    triggers.push_back(new TriggerNode("dc skip",           { NextAction("dc skip",   chatRel) }));
    triggers.push_back(new TriggerNode("dc status",         { NextAction("dc status", chatRel) }));
    triggers.push_back(new TriggerNode("dc bosses",         { NextAction("dc bosses", chatRel) }));
}
