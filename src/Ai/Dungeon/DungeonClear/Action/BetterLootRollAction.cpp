/*
 * mod-dungeon-clear — BetterLootRollAction.cpp
 */

#include "BetterLootRollAction.h"

#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

bool DungeonClearBetterLootRollAction::isUseful()
{
    // Only intercept self-bots (master == bot). A bot driven for a separate
    // human master keeps stock rolling — its vote is its own GUID, no conflict.
    if (botAI->IsRealPlayer() && DcSettings::GetBool(bot, "BetterLootRolling"))
        return false;  // bot-self: cast no vote so the human gets to roll

    return LootRollAction::isUseful();
}
