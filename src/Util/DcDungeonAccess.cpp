/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Util/DcDungeonAccess.h"

#include "AreaDefines.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"

namespace
{
    // The death knight class spell the Ebon Hold intro chain grants; the core
    // uses knowing it as the proof that a death knight has finished the
    // starting zone and may leave it (Player::TeleportTo, and again in the
    // battleground join handler).
    constexpr std::uint32_t SPELL_DEATH_GATE = 50977;

    void Append(std::string& summary, std::string const& item)
    {
        if (!summary.empty())
            summary += ", ";
        summary += item;
    }
}

namespace DcDungeonAccess
{
    bool UnlockTravel(Player* bot)
    {
        if (!bot || !bot->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_TELEPORT))
            return false;
        if (bot->GetMapId() != MAP_EBON_HOLD || bot->HasSpell(SPELL_DEATH_GATE))
            return false;

        bot->learnSpell(SPELL_DEATH_GATE);
        LOG_INFO("playerbots.dungeonclear",
                 "ACCESS {} is a death knight still on Acherus without Death Gate — taught it "
                 "{} so it can leave the starting zone",
                 bot->GetName(), SPELL_DEATH_GATE);
        return true;
    }

    std::string GrantEntry(Player* bot, std::uint32_t mapId, Difficulty difficulty)
    {
        if (!bot)
            return {};

        std::string summary;
        if (UnlockTravel(bot))
            Append(summary, Acore::StringFormat("spell {} (Death Gate)", SPELL_DEATH_GATE));

        DungeonProgressionRequirements const* ar =
            sObjectMgr->GetAccessRequirement(mapId, difficulty);
        if (!ar)
            return summary;

        // Satisfy() tests a row against the bot's ORIGINAL team, and treats a
        // TEAM_NEUTRAL row as applying to everyone. Mirror both, so we never
        // hand a bot the opposite faction's attunement.
        TeamId const team = bot->GetTeamId(true);
        auto applies = [team](ProgressionRequirement const* req)
        { return req->faction == TEAM_NEUTRAL || req->faction == team; };

        for (ProgressionRequirement const* req : ar->quests)
        {
            if (!applies(req) || bot->GetQuestRewardStatus(req->id))
                continue;
            // The requirement only ever reads GetQuestRewardStatus, so marking
            // the quest rewarded is the whole grant — no reward items, no XP,
            // no chain side effects. It persists with the character, so a
            // recycled bot pays for it once.
            bot->SetRewardedQuest(req->id);
            Append(summary, Acore::StringFormat("quest {}", req->id));
        }

        for (ProgressionRequirement const* req : ar->achievements)
        {
            if (!applies(req) || bot->HasAchieved(req->id))
                continue;
            if (AchievementEntry const* entry = sAchievementStore.LookupEntry(req->id))
            {
                bot->CompletedAchievement(entry);
                Append(summary, Acore::StringFormat("achievement {}", req->id));
            }
        }

        for (ProgressionRequirement const* req : ar->items)
        {
            if (!applies(req) || bot->HasItemCount(req->id, 1))
                continue;
            if (bot->AddItem(req->id, 1))
                Append(summary, Acore::StringFormat("item {}", req->id));
        }

        if (!summary.empty())
            LOG_INFO("playerbots.dungeonclear",
                     "ACCESS granted {} entry to map {} (difficulty {}): {}", bot->GetName(),
                     mapId, std::uint32_t(difficulty), summary);

        return summary;
    }
}
