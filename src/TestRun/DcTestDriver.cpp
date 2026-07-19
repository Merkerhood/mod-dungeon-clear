/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestDriver.h"

#include "CharacterCache.h"
#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

namespace DcTestDriver
{
    namespace
    {
        ObjectGuid _guid;           // resolved from the config name, cached
        std::string _resolvedName;  // name _guid was resolved for (conf can change)
        bool _loginIssued = false;
        bool _initialized = false;

        std::string ConfName()
        {
            return sConfigMgr->GetOption<std::string>("DungeonClear.TestRun.DriverCharacter",
                                                      "Dcdriver");
        }

        // Resolve (and re-resolve after a conf change) the driver's guid.
        ObjectGuid ResolveGuid()
        {
            std::string const name = ConfName();
            if (name.empty())
                return ObjectGuid::Empty;
            if (_guid && name == _resolvedName)
                return _guid;
            _guid = sCharacterCache->GetCharacterGuidByName(name);
            _resolvedName = name;
            _initialized = false;
            _loginIssued = false;
            return _guid;
        }

        Player* FindOnline()
        {
            if (!_guid)
                return nullptr;
            Player* p = ObjectAccessor::FindPlayer(_guid);
            return p && p->IsInWorld() ? p : nullptr;
        }
    }

    Player* Get()
    {
        ResolveGuid();
        return _initialized ? FindOnline() : nullptr;
    }

    bool EnsureOnline(std::string* whyPending)
    {
        if (Get())
            return true;

        if (!ResolveGuid())
        {
            if (whyPending)
                *whyPending = "test driver character '" + ConfName() +
                              "' not found — create it on a dedicated bot account and set "
                              "DungeonClear.TestRun.DriverCharacter";
            return false;
        }

        if (!_loginIssued)
        {
            // Masterless fake-session login (the random-bot path; the driver's
            // account is not a random account, so the rotation ignores it).
            sRandomPlayerbotMgr.AddPlayerBot(_guid, 0);
            _loginIssued = true;
            LOG_INFO("playerbots.dungeonclear", "TESTDRIVER logging in '{}' ({})",
                     _resolvedName, _guid.ToString());
        }

        if (whyPending)
            *whyPending = "test driver '" + _resolvedName +
                          "' is logging in — retry in a few seconds";
        return false;
    }

    void Tick()
    {
        if (_initialized)
        {
            // A vanished driver (kick, crash recovery) re-arms the login so the
            // next EnsureOnline can bring it back.
            if (_loginIssued && !FindOnline())
            {
                _initialized = false;
                _loginIssued = false;
            }
            return;
        }
        if (!_loginIssued)
            return;

        Player* driver = FindOnline();
        PlayerbotAI* ai = driver ? GET_PLAYERBOT_AI(driver) : nullptr;
        if (!driver || !ai)
            return;  // still loading — poll again next tick

        // One-time setup, in dependency order:
        // 1. Its own PlayerbotMgr, so GET_PLAYERBOT_MGR(driver) resolves for
        //    AddPlayerBot / LogoutPlayerBot (the AI map and mgr map are
        //    separate registries — both can exist for one guid).
        if (!GET_PLAYERBOT_MGR(driver))
            sPlayerbotsMgr.AddPlayerbotData(driver, false);

        // 2. Self-mastered: party bots' HasRealPlayerMaster() checks the
        //    master's AI IsRealPlayer() (master == bot), so this one line is
        //    what keeps the stock fast path (react delay etc.) for every run
        //    the driver issues.
        ai->SetMaster(driver);

        // 3. Neutralize. The masterless login installed the random-bot
        //    strategy set (grind/travel/rpg) — re-derive for the now
        //    self-mastered "real player" first (the .playerbots self flow),
        //    then pin it in place. GM mode also drops it from mob
        //    threat/visibility entirely.
        ai->ResetStrategies();
        ai->ChangeStrategy("+stay", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("+passive", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("+passive", BOT_STATE_COMBAT);
        driver->SetGameMaster(true);

        _initialized = true;
        LOG_INFO("playerbots.dungeonclear",
                 "TESTDRIVER ready: '{}' online (account {}), self-mastered, parked at map {} "
                 "{:.1f} {:.1f} {:.1f}",
                 driver->GetName(), driver->GetSession()->GetAccountId(), driver->GetMapId(),
                 driver->GetPositionX(), driver->GetPositionY(), driver->GetPositionZ());
    }
}
