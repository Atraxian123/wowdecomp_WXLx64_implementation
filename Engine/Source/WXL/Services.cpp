// See Services.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Engine/Source/WXL/Services.h"

#include "Engine/Source/WXL/Log.h"

#include <map>
#include <mutex>
#include <string>

namespace wxl::services
{
    namespace
    {
        struct Entry { const void* table; size_t size; };
        std::mutex g_lock;
        std::map<std::string, Entry> g_table;
    }

    bool Register(const char* name, const void* table, size_t size)
    {
        if (!name || !*name || !table) return false;
        std::lock_guard<std::mutex> lock(g_lock);
        const bool replaced = g_table.count(name) != 0;
        g_table[name] = Entry{ table, size };
        wxl::LogCat("SERVICE", "'%s' %s (%zu bytes)", name, replaced ? "replaced" : "registered", size);
        return true;
    }

    const void* Get(const char* name, size_t* size)
    {
        if (size) *size = 0;
        if (!name) return nullptr;
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_table.find(name);
        if (it == g_table.end()) return nullptr;
        if (size) *size = it->second.size;
        return it->second.table;
    }
}
