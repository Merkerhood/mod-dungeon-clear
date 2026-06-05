/*
 * mod-dungeon-clear — StayDeadAction.cpp
 */

#include "StayDeadAction.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

bool DungeonClearStayDeadAction::isUseful()
{
    // Read live (not cached) so `.reload config` and per-run overrides both take
    // effect without a restart. The dead-state "auto release" trigger fires on
    // the throttled "often" cadence, so the per-call lookup is negligible.
    if (DcSettings::GetBool(bot, "PreventBotRelease"))
        return false;  // never auto-release; bot stays a corpse until rezzed

    return AutoReleaseSpiritAction::isUseful();
}
