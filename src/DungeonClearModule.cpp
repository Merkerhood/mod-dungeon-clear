/*
 * mod-dungeon-clear — DungeonClearModule.cpp
 *
 * Drop-in glue against STOCK mod-playerbots. Two scripts:
 *
 *  1. WorldScript — on the first world-update tick (guaranteed after
 *     playerbots' OnBeforeWorldInitialized has built its shared contexts),
 *     append the four DungeonClear contexts into the engine's shared
 *     registries. Doing it on the first tick (not in our own
 *     OnBeforeWorldInitialized) sidesteps any module hook-ordering race.
 *
 *  2. PlayerScript — on login, apply the "dungeon clear chat" strategy to a
 *     bot's non-combat engine so it hears party-chat keywords (`dc on`, …).
 *     The `.dc` slash command (DungeonClearCommand.cpp) works without this.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Player.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"

#include "AiObjectContextAccess.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearActionContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearStrategyContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearTriggerContext.h"
#include "Ai/Dungeon/DungeonClear/DungeonClearValueContext.h"

class DungeonClearRegistrarWorldScript : public WorldScript
{
public:
    DungeonClearRegistrarWorldScript() : WorldScript("DungeonClearRegistrarWorldScript") {}

    void OnUpdate(uint32 /*diff*/) override
    {
        if (_registered)
            return;
        _registered = true;

        dc_access::SharedStrategyContexts()->Add(new DungeonClearStrategyContext());
        dc_access::SharedActionContexts()->Add(new DungeonClearActionContext());
        dc_access::SharedTriggerContexts()->Add(new DungeonClearTriggerContext());
        dc_access::SharedValueContexts()->Add(new DungeonClearValueContext());

        LOG_INFO("module", "mod-dungeon-clear: registered DungeonClear contexts "
                           "(strategy/action/trigger/value) into mod-playerbots.");
    }

private:
    bool _registered = false;
};

class DungeonClearLoginPlayerScript : public PlayerScript
{
public:
    DungeonClearLoginPlayerScript() : PlayerScript("DungeonClearLoginPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;

        // Only bots (and self-bot players) have a PlayerbotAI. Give them the
        // chat-keyword listener on the non-combat engine. Harmless if the
        // contexts aren't registered yet — ChangeStrategy is a no-op for an
        // unknown name, and the bot will pick it up once they are.
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
            return;

        if (!botAI->HasStrategy("dungeon clear chat", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+dungeon clear chat", BOT_STATE_NON_COMBAT);
    }
};

void AddSC_dungeon_clear_module()
{
    new DungeonClearRegistrarWorldScript();
    new DungeonClearLoginPlayerScript();
}
