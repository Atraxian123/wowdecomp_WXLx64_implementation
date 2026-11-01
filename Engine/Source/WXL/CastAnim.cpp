// WXL cast animation scaling switches. See CastAnim.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/CastAnim.h"

#include "Engine/Source/WXL/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace wxl::castanim
{
    namespace
    {
        // WXLExtendedEquipment.ini sits beside the client executable. This module lives in WXLCore, which
        // cannot call the equip modules' helper, so the lookup is repeated, as Log.cpp and Culling.cpp do.
        // Missing file, section or key: the default.
        bool IniPath(char* path, size_t cap)
        {
#ifdef _WIN32
            const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(cap));
            if (n == 0 || n >= cap) return false;
            char* slash = std::strrchr(path, '\\');
            const char* name = "WXLExtendedEquipment.ini";
            if (!slash || static_cast<size_t>(slash - path) + 1 + std::strlen(name) >= cap) return false;
            std::strcpy(slash + 1, name);
            return true;
#else
            (void)path; (void)cap;
            return false;
#endif
        }

        int IniInt(const char* section, const char* key, int def)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return def;
            return static_cast<int>(GetPrivateProfileIntA(section, key, def, path));
#else
            (void)section; (void)key;
            return def;
#endif
        }

        float IniFloat(const char* section, const char* key, float def)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            if (!IniPath(path, sizeof(path))) return def;
            char out[64] = {};
            GetPrivateProfileStringA(section, key, "", out, sizeof(out), path);
            if (!out[0]) return def;
            char* end = nullptr;
            const double v = std::strtod(out, &end);
            return (end == out) ? def : static_cast<float>(v);
#else
            (void)section; (void)key;
            return def;
#endif
        }

        struct Config
        {
            bool scalePrecast = false;
            float minSpeed = 0.5f;
            float maxSpeed = 2.0f;
            bool loopShorter = false;

            Config()
            {
                scalePrecast = IniInt("Animation", "ScalePrecastToCastTime", 0) != 0;
                minSpeed = IniFloat("Animation", "PrecastMinSpeed", 0.5f);
                maxSpeed = IniFloat("Animation", "PrecastMaxSpeed", 2.0f);
                loopShorter = IniInt("Animation", "LoopShorterAnimations", 0) != 0;

                // A zero, negative or inverted range would stall the animation or divide by zero later.
                if (!(minSpeed > 0.0f)) minSpeed = 0.5f;
                if (!(maxSpeed >= minSpeed)) maxSpeed = minSpeed;

                if (scalePrecast)
                    wxl::LogCat("ANIM", "precast animation scaled to cast time (speed %.2f..%.2f), shorter animations %s", minSpeed, maxSpeed, loopShorter ? "looped (slowed by negative haste first)" : "fitted too");
                else
                    wxl::LogCat("ANIM", "precast animation: stock behaviour ([Animation] ScalePrecastToCastTime is 0 or absent)");
            }
        };

        // Built on first use; C++11 guarantees the construction happens once even if several threads race.
        const Config& Cfg()
        {
            static const Config c;
            return c;
        }
    }

    bool ScalePrecastEnabled() { return Cfg().scalePrecast; }
    float PrecastMinSpeed() { return Cfg().minSpeed; }
    float PrecastMaxSpeed() { return Cfg().maxSpeed; }
    bool LoopShorterAnimations() { return Cfg().loopShorter; }
}
