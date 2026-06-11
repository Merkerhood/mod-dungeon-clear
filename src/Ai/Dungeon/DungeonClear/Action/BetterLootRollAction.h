/*
 * mod-dungeon-clear — BetterLootRollAction.h
 *
 * "Better Loot Rolling", improvement #1: a bot in "bot self" mode (master ==
 * bot — the human's own character running on autopilot) must NOT cast an
 * automatic Need/Greed vote on group loot. The bot and the human share one
 * character GUID, so the bot's vote is counted FOR the player and pre-empts
 * their roll dialog — a double roll. Suppressing the bot vote lets only the
 * player roll.
 *
 * Housed in this module (not in mod-playerbots) so the stock module stays
 * unedited and conflict-free on upstream pulls. The wiring is the same override
 * seam DungeonClear already uses for "auto release" (see StayDeadAction.h):
 * DungeonClearActionContext registers the "loot roll" creator name, and because
 * the engine's shared creator map keeps the LAST registration for a given name
 * (SharedNamedObjectContextList::Add) and the DungeonClear contexts are appended
 * AFTER playerbots builds its own, this creator wins for every bot of every
 * class.
 *
 * Gated by the config flag DungeonClear.BetterLootRolling (default off). When
 * the flag is off — or the bot is not a self-bot (it has a separate human
 * master) — this behaves exactly like the stock LootRollAction.
 */

#ifndef _DUNGEONCLEAR_BETTERLOOTROLLACTION_H
#define _DUNGEONCLEAR_BETTERLOOTROLLACTION_H

#include "LootRollAction.h"

class PlayerbotAI;

class DungeonClearBetterLootRollAction : public LootRollAction
{
public:
    DungeonClearBetterLootRollAction(PlayerbotAI* botAI)
        : LootRollAction(botAI, "loot roll") {}

    bool isUseful() override;
};

#endif
