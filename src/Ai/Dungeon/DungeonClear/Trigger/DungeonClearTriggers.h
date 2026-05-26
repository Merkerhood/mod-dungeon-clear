/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARTRIGGERS_H
#define _PLAYERBOT_DUNGEONCLEARTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

class DungeonClearIdleTrigger : public Trigger
{
public:
    DungeonClearIdleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear idle", 1) {}
    bool IsActive() override;
};

class DungeonClearAtBossTrigger : public Trigger
{
public:
    DungeonClearAtBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear at boss", 1) {}
    bool IsActive() override;
};

class DungeonClearBlockingTrashTrigger : public Trigger
{
public:
    DungeonClearBlockingTrashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear blocking trash", 1) {}
    bool IsActive() override;
};

class DungeonClearPartyDiedTrigger : public Trigger
{
public:
    DungeonClearPartyDiedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear party died", 1) {}
    bool IsActive() override;
};

class DungeonClearAllClearedTrigger : public Trigger
{
public:
    DungeonClearAllClearedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear all cleared", 1) {}
    bool IsActive() override;
};

// Fires only while DC is enabled AND the advance/engage path has set a stall
// reason. Drives the fallback "kill anything reachable" action.
class DungeonClearStalledTrigger : public Trigger
{
public:
    DungeonClearStalledTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear stalled", 1) {}
    bool IsActive() override;
};

// Fires on non-tank party bots when a tank in their party has DC enabled and
// the bot is too far from that tank. Redirects follow from the player master
// to the tank for the duration of the clear.
class DungeonClearFollowTankTrigger : public Trigger
{
public:
    DungeonClearFollowTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear follow tank", 1) {}
    bool IsActive() override;
};

// Fires when the cached long-path corridor crosses a closed
// `GAMEOBJECT_TYPE_DOOR`. The bot stops advancing and stalls with a
// specific reason in party chat so the human player can open the door.
class DungeonClearDoorBlockedTrigger : public Trigger
{
public:
    DungeonClearDoorBlockedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dungeon clear door blocked", 1) {}
    bool IsActive() override;
};

#endif
