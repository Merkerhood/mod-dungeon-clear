/*
 * mod-dungeon-clear — DcSettings.h
 *
 * Accessor for DungeonClear tunables. Resolution order on every read:
 *
 *     per-run override  ->  mod_dungeon_clear.conf  ->  registry default
 *
 * The per-run override layer is keyed by the run's leader-tank GUID
 * (DcLeaderSignal::FindLeaderTank), so each dungeon run can carry its own
 * settings pushed from the companion addon while the conf file stays the
 * server-wide default. Reads are lazy (no caching), so an override applied
 * mid-run takes effect on the next tick with no reload.
 *
 * Only player-facing registry rows consult the override store; server-only rows
 * (e.g. PathCenter*) go straight to conf, which keeps them safe to read from the
 * map-update worker threads that run corridor centering.
 *
 * See DcSettingsRegistry.h for the setting table.
 */

#ifndef _DUNGEON_CLEAR_DC_SETTINGS_H
#define _DUNGEON_CLEAR_DC_SETTINGS_H

#include <string>

#include "ObjectGuid.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettingsRegistry.h"

class Player;

namespace DcSettings
{
    // Effective value for a run, keyed by the leader-tank GUID. Pass
    // ObjectGuid::Empty (or a server-only key) to read the plain conf/default.
    bool   GetBool (ObjectGuid runOwner, char const* key);
    int32  GetInt  (ObjectGuid runOwner, char const* key);
    uint32 GetUInt (ObjectGuid runOwner, char const* key);
    float  GetFloat(ObjectGuid runOwner, char const* key);

    // Convenience overloads that resolve the run owner from any bot in the run
    // (FindLeaderTank). For server-only keys the owner lookup is skipped, so
    // these are cheap to call from non-player contexts too.
    bool   GetBool (Player* bot, char const* key);
    int32  GetInt  (Player* bot, char const* key);
    uint32 GetUInt (Player* bot, char const* key);
    float  GetFloat(Player* bot, char const* key);

    // Validate `key` against the registry (must exist AND be player-facing),
    // clamp `rawValue` to the registry range, normalise discrete types, and
    // store it for `runOwner`. Returns false (and fills `err` if non-null) when
    // the key is unknown or not overridable. Never trusts the raw value.
    bool SetOverride(ObjectGuid runOwner, std::string const& key,
                     double rawValue, std::string* err = nullptr);

    // Drop a single override (empty key = every override for the run), reverting
    // to the conf/default. ClearRun wipes the whole run — call it when a run
    // ends so leader GUIDs don't accumulate.
    void ResetOverride(ObjectGuid runOwner, std::string const& key);
    void ClearRun(ObjectGuid runOwner);

    // True if `runOwner` has an explicit override for `key` (vs. inheriting the
    // conf/default). Used by the addon sync payload to flag overridden values.
    bool HasOverride(ObjectGuid runOwner, char const* key);

    // Effective value as a double, for building the addon sync payload by
    // iterating kDcSettings without a type switch at the call site.
    double GetEffectiveRaw(ObjectGuid runOwner, DcSettingDef const& def);
}

#endif
