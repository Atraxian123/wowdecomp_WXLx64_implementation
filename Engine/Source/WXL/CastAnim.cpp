// WXL cast animation scaling switches. See CastAnim.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/CastAnim.h"

#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace wxl::castanim
{
    namespace
    {
        int   IniInt(const char* section, const char* key, int def) { return wxl::ini::GetInt(section, key, def); }
        float IniFloat(const char* section, const char* key, float def) { return wxl::ini::GetFloat(section, key, def); }

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

        // The switches as the ini has them now. Rebuilt when the ini changes (a save, or a console override); the old
        // Config is left in place, never freed, because a thread that fetched it a moment ago may still be reading it.
        const Config& Cfg()
        {
            static std::atomic<const Config*> current{nullptr};
            static std::atomic<uint32_t> seen{0xFFFFFFFFu};
            static std::mutex m;
            const uint32_t gen = wxl::ini::Generation();
            if (seen.load(std::memory_order_acquire) != gen || !current.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(m);
                if (seen.load(std::memory_order_relaxed) != gen || !current.load(std::memory_order_relaxed))
                {
                    current.store(new Config(), std::memory_order_release);
                    seen.store(gen, std::memory_order_release);
                }
            }
            return *current.load(std::memory_order_acquire);
        }
    }

    bool ScalePrecastEnabled() { return Cfg().scalePrecast; }
    float PrecastMinSpeed() { return Cfg().minSpeed; }
    float PrecastMaxSpeed() { return Cfg().maxSpeed; }
    bool LoopShorterAnimations() { return Cfg().loopShorter; }
}
