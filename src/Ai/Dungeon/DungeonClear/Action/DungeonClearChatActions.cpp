/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearChatActions.h"

#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Playerbots.h"

namespace
{
    bool IsAuthorized(Player* bot, Event const& event)
    {
        Player* owner = const_cast<Event&>(event).getOwner();
        if (!owner)
            return false;
        // Reject true bots, but allow self-bot players (a real player whose
        // own PlayerbotAI has master == bot). Without this exception the
        // moment a player enables bot self mode they would be silently
        // locked out of every dc command.
        if (PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner))
            if (!ownerAI->IsRealPlayer())
                return false;
        if (!bot || !bot->GetGroup())
            return false;
        return bot->GetGroup()->IsMember(owner->GetGUID());
    }

    bool AnyPartyMemberDead(Player* bot)
    {
        if (!bot)
            return false;
        if (bot->isDead())
            return true;
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot)
                continue;
            if (member->GetMapId() != bot->GetMapId())
                continue;
            if (member->isDead())
                return true;
        }
        return false;
    }

    std::string FirstDeadName(Player* bot)
    {
        if (!bot)
            return "Someone";
        if (bot->isDead())
            return bot->GetName();
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot)
                    continue;
                if (member->GetMapId() != bot->GetMapId())
                    continue;
                if (member->isDead())
                    return member->GetName();
            }
        }
        return "Someone";
    }
}

bool DcOnAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to enable dungeon clear");
        return false;
    }
    if (!PlayerbotAI::IsTank(bot))
    {
        botAI->TellError("I'm not the tank — ask the tank bot instead.");
        return false;
    }
    if (!bot->GetMap() || !bot->GetMap()->IsDungeon())
    {
        botAI->TellError("Not in a dungeon.");
        return false;
    }

    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");
    if (bosses.empty())
    {
        botAI->TellError("No bosses found for this map.");
        return false;
    }

    if (AnyPartyMemberDead(bot))
    {
        botAI->TellError(FirstDeadName(bot) + " is dead — rez and try again.");
        return false;
    }

    // Reset transient state and enable.
    context->GetValue<bool>("dungeon clear enabled")->Set(true);
    context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get().clear();
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    // Force a fresh long-path build on the first Advance tick. Without
    // this, a stale path from a previous `dc on`/`dc off` cycle would
    // be reused.
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    std::string const target = next.has_value() ? next->name : "the next boss";
    botAI->SayToParty("Dungeon clear enabled. Heading to " + target + ".");
    return true;
}

bool DcOffAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to disable dungeon clear");
        return false;
    }
    context->GetValue<bool>("dungeon clear enabled")->Set(false);
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    // Cancel any in-flight advance/engage MoveTo so the bot actually stops
    // walking when the player says `dc off`. Without this, a previously
    // queued MOVEMENT_NORMAL/MOVEMENT_COMBAT spline keeps running to its
    // endpoint even though the strategy is now disabled — the bot looks
    // like it's ignoring the off command for several seconds.
    if (bot)
    {
        if (bot->isMoving())
            bot->StopMoving();
        if (MotionMaster* mm = bot->GetMotionMaster())
            mm->Clear();
    }

    botAI->SayToParty("Dungeon clear disabled.");
    return true;
}

bool DcSkipAction::Execute(Event event)
{
    if (!IsAuthorized(bot, event))
    {
        botAI->TellError("Not authorized to skip");
        return false;
    }
    if (!AI_VALUE(bool, "dungeon clear enabled"))
    {
        botAI->TellError("Dungeon clear is not enabled.");
        return false;
    }

    std::optional<DungeonBossInfo> current = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    if (!current.has_value())
    {
        botAI->TellError("No current boss to skip.");
        return false;
    }

    std::unordered_set<uint32>& skipped =
        context->GetValue<std::unordered_set<uint32>&>("dungeon clear skipped")->Get();
    skipped.insert(current->entry);

    // New target gets a clean slate.
    context->GetValue<uint32>("dungeon clear stuck count")->Set(0u);
    context->GetValue<uint32>("dungeon clear last target entry")->Set(0u);
    context->GetValue<std::string&>("dungeon clear stall reason")->Get().clear();
    context->GetValue<std::string&>("dungeon clear last said reason")->Get().clear();
    context->GetValue<ObjectGuid>("dungeon clear fallback target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("dungeon clear engage trash target")->Set(ObjectGuid::Empty);
    // Invalidate the long-path cache so the next Advance tick rebuilds
    // for the new target boss.
    context->GetValue<uint32>("dungeon clear long path target")->Set(0u);
    context->GetValue<uint32>("dungeon clear long path expires")->Set(0u);
    context->GetValue<uint32>("dungeon clear current hop")->Set(0u);
    context->GetValue<ChunkedPathfinder::Result&>("dungeon clear long path")->Reset();
    context->GetValue<DungeonFollowerState&>("dungeon clear follower state")->Get() = DungeonFollowerState{};

    // Force the cached next-boss value to recompute on next call.
    context->GetValue<std::optional<DungeonBossInfo>>("next dungeon boss")->Reset();
    std::optional<DungeonBossInfo> after = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");

    if (after.has_value())
    {
        botAI->SayToParty("Skipped " + current->name + ". Heading to " + after->name + ".");
    }
    else
    {
        botAI->SayToParty("Skipped " + current->name + ". No bosses left — disabling.");
        context->GetValue<bool>("dungeon clear enabled")->Set(false);
    }
    return true;
}

bool DcStatusAction::Execute(Event /*event*/)
{
    bool const enabled = AI_VALUE(bool, "dungeon clear enabled");
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, "next dungeon boss");
    auto const& skipped = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");
    std::string const& stall = AI_VALUE(std::string&, "dungeon clear stall reason");

    std::ostringstream msg;
    msg << "Dungeon clear: " << (enabled ? "on" : "off")
        << ". Next boss: " << (next.has_value() ? next->name : "none")
        << ". Skipped: " << skipped.size() << ".";
    if (!stall.empty())
        msg << " Stalled: " << stall;
    botAI->SayToParty(msg.str());
    return true;
}

bool DcBossesAction::Execute(Event /*event*/)
{
    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, "dungeon bosses");
    if (bosses.empty())
    {
        botAI->SayToParty("No bosses found for this map.");
        return true;
    }

    auto const& skipped = AI_VALUE(std::unordered_set<uint32>&, "dungeon clear skipped");
    for (DungeonBossInfo const& info : bosses)
    {
        std::ostringstream line;
        line << info.encounterIndex << ". " << info.name
             << " @ (" << static_cast<int>(info.x) << ", "
             << static_cast<int>(info.y) << ", "
             << static_cast<int>(info.z) << ") [";

        if (skipped.count(info.entry))
            line << "skipped";
        else if (!DungeonClearUtil::IsCreaturePresentOnMap(bot, info.entry))
            line << "missing";
        else if (Creature* c = DungeonClearUtil::FindLiveCreatureOnMap(bot, info.entry))
        {
            (void)c;
            line << "alive";
        }
        else
            line << "dead";
        line << "]";

        botAI->SayToParty(line.str());
    }
    return true;
}
