/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTDRIVER_H
#define _PLAYERBOT_DCTESTDRIVER_H

#include <string>

#include "ObjectGuid.h"

class Player;

// Headless GM stand-in for `.dc test` issued from the console / dashboard.
//
// The whole test harness leans on an issuing GM Player*: the party bots log in
// under the GM's PlayerbotMgr and account, keep the GM as playerbots MASTER
// (HasRealPlayerMaster() gates the stock fast path — react delay, AoE
// avoidance; the S1062 fix), and FindGm() liveness-gates every stage. So a
// console start needs a real Player object, not a code path around one.
//
// This provides it: a dedicated character (config
// DungeonClear.TestRun.DriverCharacter, on its own bot account) logged in
// headlessly through the playerbots fake-session machinery
// (sRandomPlayerbotMgr.AddPlayerBot(guid, 0) — the masterless login path),
// then made to pass for a real player master:
//   * its own PlayerbotAI is SELF-MASTERED (SetMaster(driver)), so for every
//     party bot HasRealPlayerMaster() == masterBotAI->IsRealPlayer() == true —
//     the react-throttle fast path holds exactly as with a human GM;
//   * it gets its own PlayerbotMgr (PlayerbotsMgr::AddPlayerbotData(p, false);
//     the AI map and mgr map are separate, so one character can hold both),
//     which is what AddPlayerBot/LogoutPlayerBot run through;
//   * its AI is neutralized (stay + passive, GM mode on) so the character
//     never wanders, fights, or draws aggro while parked.
//
// The driver's account must NOT be in AiPlayerbot.RandomBotAccounts (the
// random-bot rotation would manage/log it out) and must not be an addclass
// pool account (pool chars get claimed as party slots).
//
// All world-thread only.
namespace DcTestDriver
{
    // The driver, once online and initialized; nullptr otherwise. Callers use
    // this as the issuing GM for Start().
    Player* Get();

    // Kick off the headless login if the driver isn't online yet. Returns
    // true when Get() is already usable; false with *whyPending set while the
    // login is in flight or misconfigured (unknown character name).
    bool EnsureOnline(std::string* whyPending);

    // Poll the async login and run the one-time initialization (mgr +
    // self-master + neutralize) once the character is in world. Cheap no-op
    // when idle or already initialized; call every world tick.
    void Tick();
}

#endif  // _PLAYERBOT_DCTESTDRIVER_H
