/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearStrategy.h"

#include "Ai/Dungeon/DungeonClear/Multiplier/DungeonClearMultiplier.h"
#include "Playerbots.h"

void DungeonClearStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Highest priority: bail out on death.
    triggers.push_back(new TriggerNode(
        "dungeon clear party died",
        { NextAction("dungeon clear disable on death", 100.0f) }));

    // All bosses cleared — congratulate and disable.
    triggers.push_back(new TriggerNode(
        "dungeon clear all cleared",
        { NextAction("dungeon clear disable on cleared", 50.0f) }));

    // Within engage range of next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear at boss",
        { NextAction("dungeon clear engage boss", 30.0f) }));

    // Blocking trash on the path to the next boss.
    triggers.push_back(new TriggerNode(
        "dungeon clear blocking trash",
        { NextAction("dungeon clear engage trash", 25.0f) }));

    // Stalled fallback: only fires when Advance/EngageBoss has set a stall
    // reason because no path to the next boss exists. Sits above the default
    // advance (15) so the fallback kill wins, and below at-boss (30) and
    // blocking-trash (25) so a viable boss/trash pull still preempts.
    triggers.push_back(new TriggerNode(
        "dungeon clear stalled",
        { NextAction("dungeon clear clear stalled", 20.0f) }));

    // Door blocking the corridor: stall with a specific message. Sits
    // above advance (15) but below the engage triggers so a hostile in the
    // doorway still gets pulled first; otherwise the bot stops and waits
    // for the door to be opened.
    triggers.push_back(new TriggerNode(
        "dungeon clear door blocked",
        { NextAction("dungeon clear door blocked", 22.0f) }));

    // Default: walk toward the next boss. Lowest of the bunch but above
    // grind (4) / new rpg (11). Wander strategies are also suppressed by
    // DungeonClearMultiplier while enabled.
    triggers.push_back(new TriggerNode(
        "dungeon clear idle",
        { NextAction("dungeon clear advance", 15.0f) }));

    // Non-tank bots in the tank's party redirect their follow target to the
    // tank while DC is on. Relevance above the default follow (1.0) so it
    // preempts the usual master-follow behavior.
    triggers.push_back(new TriggerNode(
        "dungeon clear follow tank",
        { NextAction("dungeon clear follow tank", 25.0f) }));

    // Chat keyword triggers live in the sibling `dungeon clear chat`
    // strategy. That strategy is registered on the combat, non-combat, and
    // dead engines so `dc off` is processed mid-pull instead of being held
    // until the bot returns to non-combat.
}

void DungeonClearStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new DungeonClearMultiplier(botAI));
}
