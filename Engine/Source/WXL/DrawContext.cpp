// Reads the one WXLSetup.ini switch the M2 draw context depends on. See DrawContext.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/DrawContext.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>

namespace wxl::events
{
    namespace
    {
        // WXLSetup.ini sits in the WXL folder beside the client executable; a copy left beside the executable (the
        // old place) is still read. The lookup is repeated here because WXLCore cannot call the modules' helper, as
        // Culling.cpp does. Missing file, section or key: the default.
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

    bool BlendedM2BatchesWanted()
    {
        static const bool wanted = IniInt("Outline", "TransparentUnits", 0) != 0;
        return wanted;
    }
}
