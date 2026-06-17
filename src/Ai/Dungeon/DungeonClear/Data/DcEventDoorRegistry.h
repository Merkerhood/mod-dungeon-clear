/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCEVENTDOORREGISTRY_H
#define _PLAYERBOT_DCEVENTDOORREGISTRY_H

#include "Common.h"

// Per-ENTRY list of door gameobjects that are SCRIPT-ONLY: the live client
// refuses a direct player open and ONLY an in-game event opens them, even though
// their template (an empty lock-85, the same template as plenty of plainly
// clickable doors) reads as openable to BotCanOpenDoorLikePlayer / DcDoorPolicy.
// A bot generic-Use()ing one of these toggles the server GO state while the
// client still treats the door as shut — a desync — and it also skips the
// intended event (e.g. Shadowfang Keep's courtyard door, which only opens when a
// freed prisoner walks over and unlocks it).
//
// This is DELIBERATELY keyed by GO ENTRY, not by lock id: lock 85 is shared with
// many doors bots SHOULD open (Deadmines Factory/Foundry/Mast Room, etc.), so a
// lock-level rule would break them. Keep this list to doors verified to be
// script/event-opened only; the door-blocked action consults it before deciding
// it is "entitled" to open a door, and leaves a listed door for the events
// framework or the human instead.
namespace DcEventDoorRegistry
{
    inline bool IsScriptOnly(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 18895:  // Shadowfang Keep — Courtyard Door (freed-prisoner event)
                return true;
            default:
                return false;
        }
    }

    // The MIRROR-IMAGE special case: door gameobjects carrying NO lock at all
    // (template lockId 0) that a player nonetheless opens by simply clicking
    // them — ordinary traversal gates the dungeon expects you to walk through.
    //
    // BotCanOpenDoorLikePlayer otherwise refuses every lock-free door, because
    // lockId 0 is ALSO the shape of script/event seals the bot must not pop
    // (Uldaman's Seal of Khaz'Mul, lock-free and only opened by the keystone
    // event, isn't flagged GO_FLAG_NOT_SELECTABLE until its encounter is done,
    // so the generic flag screen can't tell them apart). We can't relax the
    // lock-free rule wholesale; instead we allowlist the entries verified in
    // the world DB to be plain clickable doors — no ScriptName, no AIName, no
    // instance-script GO-state control, no SmartAI.
    //
    // Scholomance's Iron Gates (175611-175618, 175620) and plain interior Doors
    // (175610, 175619) are exactly this: lock-free, scriptless room-to-room
    // gates the player clicks open. (The dungeon's *event* gates — Kirtonos
    // 175570 and the seven Gandling gates 177371-177377 — are deliberately
    // EXCLUDED; the instance script drives their state.)
    inline bool IsLockFreeClickable(uint32 goEntry)
    {
        switch (goEntry)
        {
            // Scholomance — interior traversal gates/doors (map 289)
            case 175610:  // Door
            case 175611:  // Iron Gate
            case 175612:  // Iron Gate
            case 175613:  // Iron Gate
            case 175614:  // Iron Gate
            case 175615:  // Iron Gate
            case 175616:  // Iron Gate
            case 175617:  // Iron Gate
            case 175618:  // Iron Gate
            case 175619:  // Door
            case 175620:  // Iron Gate
                return true;
            default:
                return false;
        }
    }
}

#endif
