/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARPARTYTANKVALUE_H
#define _PLAYERBOT_DUNGEONCLEARPARTYTANKVALUE_H

#include "Value.h"

class PlayerbotAI;
class Player;

// Returns the party tank whose dungeon-clear mode is currently enabled, or
// nullptr. Used by non-tank party bots to redirect their follow target to the
// tank for the duration of the clear instead of trailing the player master.
class DungeonClearPartyTankValue : public CalculatedValue<Player*>
{
public:
    DungeonClearPartyTankValue(PlayerbotAI* botAI)
        : CalculatedValue<Player*>(botAI, "dungeon clear party tank", 2)
    {
    }

protected:
    Player* Calculate() override;
};

#endif
