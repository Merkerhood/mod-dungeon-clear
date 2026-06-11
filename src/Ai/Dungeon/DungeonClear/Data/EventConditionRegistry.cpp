/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "EventConditionRegistry.h"

#include <unordered_map>

#include "Creature.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "Player.h"

namespace
{
    // --- Shadowfang Keep: courtyard door (condition 1) --------------------
    // The Courtyard Door (GO 18895) blocks progression past the entry rooms and
    // is opened not by the party but by a freed prisoner: the bot must talk to
    // Sorcerer Ashcrombe (3850, Alliance) or Deathstalker Adamant (3849, Horde)
    // and pick "Please unlock the courtyard door." The event is DUE while the
    // door is still shut (GO_STATE_READY) AND a prisoner is alive to ask. Once a
    // human (or the bot's gossip) opens the door this reads false and the event
    // latches done.
    constexpr uint32 SFK_COURTYARD_DOOR = 18895;
    constexpr uint32 SFK_PRISONER_ASHCROMBE = 3850;
    constexpr uint32 SFK_PRISONER_ADAMANT = 3849;
    // Door / prisoner sit near the entry; scan generously so the condition is
    // true from the moment the party is anywhere in the early keep.
    constexpr float SFK_SCAN = 200.0f;

    bool SfkCourtyardDoorShut(Player* bot, AiObjectContext* /*context*/)
    {
        GameObject* door = bot->FindNearestGameObject(SFK_COURTYARD_DOOR, SFK_SCAN);
        if (!door || door->GetGoState() != GO_STATE_READY)
            return false;  // no door found, or already open

        // A prisoner must be alive to open it; if both are dead the gate cannot
        // be opened by gossip and we leave it to the door-blocked fallback.
        Creature* a = bot->FindNearestCreature(SFK_PRISONER_ASHCROMBE, SFK_SCAN, /*alive*/ true);
        Creature* b = bot->FindNearestCreature(SFK_PRISONER_ADAMANT, SFK_SCAN, /*alive*/ true);
        return a != nullptr || b != nullptr;
    }

    // conditionId -> predicate. Add a row and reference its id from a Conditional
    // event's .Conditional(id) in DungeonEventRegistry.
    std::unordered_map<uint32, EventConditionRegistry::Condition> const& Conditions()
    {
        static std::unordered_map<uint32, EventConditionRegistry::Condition> const kConditions = {
            { 1, &SfkCourtyardDoorShut },
        };
        return kConditions;
    }
}

bool EventConditionRegistry::Evaluate(uint32 id, Player* bot, AiObjectContext* context)
{
    if (id == 0 || !bot)
        return false;

    auto const& conditions = Conditions();
    auto it = conditions.find(id);
    if (it == conditions.end() || !it->second)
        return false;

    return it->second(bot, context);
}

bool EventConditionRegistry::Has(uint32 id)
{
    return id != 0 && Conditions().count(id) != 0;
}
