/*
 * mod-dungeon-clear — DungeonClearCommand.cpp
 *
 * Slash command `.dc on|off|skip|status|bosses`. A convenience entry point that
 * works with zero config (unlike the chat keywords, which need the
 * "dungeon clear chat" strategy applied — see DungeonClearModule.cpp).
 *
 * Each subcommand dispatches the matching DungeonClear action ("dc on", …) to
 * the issuing player's tank bot(s) via PlayerbotAI::DoSpecificAction. The
 * actions already self-authorize (owner must be a real player in the bot's
 * group) and self-gate (e.g. `dc on` is tank-only), so we carry the issuing
 * player as the Event owner and let the existing action logic decide.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Group.h"
#include "Player.h"

#include "Event.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

using namespace Acore::ChatCommands;

namespace
{
    // Dispatch "dc <sub>" to every tank bot in the issuer's group. Returns the
    // number of bots that handled it; 0 means "no tank bot found".
    uint32 DispatchToTankBots(Player* issuer, std::string const& action)
    {
        if (!issuer)
            return 0;

        Group* group = issuer->GetGroup();
        if (!group)
            return 0;

        uint32 dispatched = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
            if (!botAI)
                continue;
            if (!PlayerbotAI::IsTank(member))
                continue;

            botAI->DoSpecificAction(action, Event("dc", "", issuer));
            ++dispatched;
        }
        return dispatched;
    }

    bool RunDcCommand(ChatHandler* handler, std::string const& action)
    {
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!issuer)
        {
            handler->SendSysMessage("This command must be used in-game.");
            return true;
        }

        if (!DispatchToTankBots(issuer, action))
            handler->SendSysMessage("No tank bot found in your group.");

        return true;
    }
}

class dungeon_clear_command_script : public CommandScript
{
public:
    dungeon_clear_command_script() : CommandScript("dungeon_clear_command_script") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dcTable =
        {
            { "on",     HandleOn,     SEC_PLAYER, Console::No },
            { "off",    HandleOff,    SEC_PLAYER, Console::No },
            { "skip",   HandleSkip,   SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
            { "bosses", HandleBosses, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable root = { { "dc", dcTable } };
        return root;
    }

    static bool HandleOn(ChatHandler* handler)     { return RunDcCommand(handler, "dc on"); }
    static bool HandleOff(ChatHandler* handler)    { return RunDcCommand(handler, "dc off"); }
    static bool HandleSkip(ChatHandler* handler)   { return RunDcCommand(handler, "dc skip"); }
    static bool HandleStatus(ChatHandler* handler) { return RunDcCommand(handler, "dc status"); }
    static bool HandleBosses(ChatHandler* handler) { return RunDcCommand(handler, "dc bosses"); }
};

void AddSC_dungeon_clear_command()
{
    new dungeon_clear_command_script();
}
