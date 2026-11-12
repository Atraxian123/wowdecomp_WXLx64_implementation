// Ground decals (projected textures): which WMO faces they may land on. See DecalQuery.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/DecalQuery.h"

#include "Engine/Source/WXL/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>

namespace wxl::decals
{
    namespace
    {
        // WXLSetup.ini sits in the WXL folder beside the client executable; a copy left beside the executable is still
        // read. The lookup is repeated here as in the other WXLCore files that read the ini.
        int IniInt(const char* section, const char* key, int def)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
            if (n == 0 || n >= sizeof(path)) return def;
            char* slash = std::strrchr(path, '\\');
            const char* name = "WXLSetup.ini";
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= sizeof(path)) return def;
            std::strcpy(slash + 1, name);
            char inWxl[MAX_PATH] = {};
            if (static_cast<size_t>(slash - path) + 5 + std::strlen(name) < sizeof(inWxl))
            {
                std::snprintf(inWxl, sizeof(inWxl), "%.*sWXL\\%s", static_cast<int>(slash - path) + 1, path, name);
                if (GetFileAttributesA(inWxl) != INVALID_FILE_ATTRIBUTES) std::strcpy(path, inWxl);
            }
            return static_cast<int>(GetPrivateProfileIntA(section, key, def, path));
#else
            (void)section; (void)key;
            return def;
#endif
        }
    }

    bool WmoCollisionFaces()
    {
        static const bool on = [] {
            const bool v = IniInt("Decals", "WmoCollisionFaces", 1) != 0;
            wxl::LogCat("DECAL", "ground decals: WMO faces flagged both render and collision %s ([Decals] WmoCollisionFaces)", v ? "count" : "are skipped, as in the stock client");
            return v;
        }();
        return on;
    }
}
