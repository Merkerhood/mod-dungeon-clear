/*
 * mod-dungeon-clear — DcSettings.cpp
 */

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Config.h"
#include "Log.h"
#include "Player.h"

#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"

namespace
{
    // Per-run override store, keyed by the run's leader-tank GUID. Values are
    // stored as doubles already clamped/normalised by SetOverride. Accessed only
    // from the map thread (config reads during the AI tick, and the addon hook's
    // OnPlayerBeforeSendChatMessage, both run there), so no lock is needed — and
    // the worker-thread centering path never reaches here because its keys are
    // server-only (see GetRaw).
    std::unordered_map<ObjectGuid, std::unordered_map<std::string, double>> g_overrides;

    std::string FullKey(char const* keySuffix)
    {
        return std::string("DungeonClear.") + keySuffix;
    }

    // conf -> registry default, returned as a double regardless of type.
    double ConfValue(DcSettingDef const& d)
    {
        std::string const full = FullKey(d.key);
        switch (d.type)
        {
            case DcType::Bool:
                return sConfigMgr->GetOption<bool>(full, d.defVal != 0.0) ? 1.0 : 0.0;
            case DcType::UInt:
                return static_cast<double>(
                    sConfigMgr->GetOption<uint32>(full, static_cast<uint32>(d.defVal)));
            case DcType::Int:
                return static_cast<double>(
                    sConfigMgr->GetOption<int32>(full, static_cast<int32>(d.defVal)));
            case DcType::Float:
            default:
                return static_cast<double>(
                    sConfigMgr->GetOption<float>(full, static_cast<float>(d.defVal)));
        }
    }

    // The full resolution chain for one registry entry.
    double GetRaw(ObjectGuid owner, DcSettingDef const& d)
    {
        if (d.playerFacing && !owner.IsEmpty())
        {
            auto const runIt = g_overrides.find(owner);
            if (runIt != g_overrides.end())
            {
                auto const keyIt = runIt->second.find(d.key);
                if (keyIt != runIt->second.end())
                    return keyIt->second;  // already clamped at SetOverride time
            }
        }
        return ConfValue(d);
    }

    // Resolve the run owner from any bot in the run. Skipped (Empty) for
    // server-only keys so non-player callers don't pay the group walk.
    ObjectGuid OwnerForBot(Player* bot, DcSettingDef const& d)
    {
        if (!d.playerFacing || !bot)
            return ObjectGuid::Empty;

        Player* leader = DcLeaderSignal::FindLeaderTank(bot);
        return leader ? leader->GetGUID() : ObjectGuid::Empty;
    }
}

namespace DcSettings
{
    bool GetBool(ObjectGuid runOwner, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return GetRaw(runOwner, *d) != 0.0;
        return sConfigMgr->GetOption<bool>(FullKey(key), false);
    }

    int32 GetInt(ObjectGuid runOwner, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<int32>(std::lround(GetRaw(runOwner, *d)));
        return sConfigMgr->GetOption<int32>(FullKey(key), 0);
    }

    uint32 GetUInt(ObjectGuid runOwner, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<uint32>(std::lround(GetRaw(runOwner, *d)));
        return sConfigMgr->GetOption<uint32>(FullKey(key), 0);
    }

    float GetFloat(ObjectGuid runOwner, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<float>(GetRaw(runOwner, *d));
        return sConfigMgr->GetOption<float>(FullKey(key), 0.0f);
    }

    bool GetBool(Player* bot, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return GetRaw(OwnerForBot(bot, *d), *d) != 0.0;
        return sConfigMgr->GetOption<bool>(FullKey(key), false);
    }

    int32 GetInt(Player* bot, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<int32>(std::lround(GetRaw(OwnerForBot(bot, *d), *d)));
        return sConfigMgr->GetOption<int32>(FullKey(key), 0);
    }

    uint32 GetUInt(Player* bot, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<uint32>(std::lround(GetRaw(OwnerForBot(bot, *d), *d)));
        return sConfigMgr->GetOption<uint32>(FullKey(key), 0);
    }

    float GetFloat(Player* bot, char const* key)
    {
        if (DcSettingDef const* d = FindDcSetting(key))
            return static_cast<float>(GetRaw(OwnerForBot(bot, *d), *d));
        return sConfigMgr->GetOption<float>(FullKey(key), 0.0f);
    }

    bool SetOverride(ObjectGuid runOwner, std::string const& key, double rawValue,
                     std::string* err)
    {
        DcSettingDef const* d = FindDcSetting(key);
        if (!d)
        {
            if (err)
                *err = "unknown setting";
            return false;
        }
        if (!d->playerFacing)
        {
            if (err)
                *err = "setting is not overridable";
            return false;
        }
        if (runOwner.IsEmpty())
        {
            if (err)
                *err = "no active dungeon run";
            return false;
        }

        double v = std::clamp(rawValue, d->minVal, d->maxVal);
        // Discrete types snap to whole numbers (bool collapses to 0/1 via clamp).
        if (d->type != DcType::Float)
            v = std::round(v);

        g_overrides[runOwner][d->key] = v;
        LOG_DEBUG("playerbots.dungeonclear",
                  "DcSettings: override {} = {} for run {}", d->key, v,
                  runOwner.ToString());
        return true;
    }

    void ResetOverride(ObjectGuid runOwner, std::string const& key)
    {
        auto const runIt = g_overrides.find(runOwner);
        if (runIt == g_overrides.end())
            return;

        if (key.empty())
        {
            g_overrides.erase(runIt);
            return;
        }

        runIt->second.erase(key);
        if (runIt->second.empty())
            g_overrides.erase(runIt);
    }

    void ClearRun(ObjectGuid runOwner)
    {
        g_overrides.erase(runOwner);
    }

    bool HasOverride(ObjectGuid runOwner, char const* key)
    {
        auto const runIt = g_overrides.find(runOwner);
        if (runIt == g_overrides.end())
            return false;
        return runIt->second.find(key) != runIt->second.end();
    }

    double GetEffectiveRaw(ObjectGuid runOwner, DcSettingDef const& def)
    {
        return GetRaw(runOwner, def);
    }
}
