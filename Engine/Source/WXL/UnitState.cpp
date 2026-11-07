// See UnitState.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/UnitState.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace wxl::unitstate
{
    namespace
    {
        struct Entry
        {
            const void* unit;
            const void* world;
            const void* mount;
            float depthRatio;
        };

        std::mutex g_lock;
        std::vector<Entry> g_swimming;   // a few dozen at most: the units in water that the client is updating
    }

    void NoteUnit(const void* unit, const void* worldModel, const void* mountModel, bool swimming, float depthRatio)
    {
        if (!unit) return;
        std::lock_guard<std::mutex> g(g_lock);
        auto it = std::find_if(g_swimming.begin(), g_swimming.end(), [&](const Entry& e) { return e.unit == unit; });
        if (!swimming)
        {
            if (it != g_swimming.end()) g_swimming.erase(it);
            return;
        }
        if (it != g_swimming.end()) { it->world = worldModel; it->mount = mountModel; it->depthRatio = depthRatio; }
        else g_swimming.push_back(Entry{ unit, worldModel, mountModel, depthRatio });
    }

    void ForgetUnit(const void* unit)
    {
        if (!unit) return;
        std::lock_guard<std::mutex> g(g_lock);
        g_swimming.erase(std::remove_if(g_swimming.begin(), g_swimming.end(), [&](const Entry& e) { return e.unit == unit; }),
                         g_swimming.end());
    }

    bool IsSwimming(const void* model)
    {
        if (!model) return false;
        std::lock_guard<std::mutex> g(g_lock);
        for (const Entry& e : g_swimming)
            if (e.world == model || e.mount == model) return true;
        return false;
    }

    float WaterDepthRatio(const void* model)
    {
        if (!model) return -1.0f;
        std::lock_guard<std::mutex> g(g_lock);
        for (const Entry& e : g_swimming)
            if (e.world == model || e.mount == model) return e.depthRatio;
        return -1.0f;
    }
}
