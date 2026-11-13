// The animation of the second and third texture of a multi-textured emitter. See LayerScroll.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/LayerScroll.h"

#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace wxl::layerscroll
{
    namespace
    {
        Speeds ReadSection(const char* section)
        {
            Speeds s;
            static const char* const kX[kMovingTextures]   = {"Texture2XTranslation", "Texture3XTranslation", "Texture4XTranslation"};
            static const char* const kY[kMovingTextures]   = {"Texture2YTranslation", "Texture3YTranslation", "Texture4YTranslation"};
            static const char* const kTurn[kMovingTextures] = {"Texture2Rotation", "Texture3Rotation", "Texture4Rotation"};
            for (uint32_t i = 0; i < kMovingTextures; ++i)
            {
                s.x[i] = wxl::ini::GetFloat(section, kX[i], 0.0f);
                s.y[i] = wxl::ini::GetFloat(section, kY[i], 0.0f);
                s.turn[i] = wxl::ini::GetFloat(section, kTurn[i], 0.0f);
            }
            return s;
        }

        // One section's speeds, re-read when the ini changes. The old Speeds are never freed: a thread that fetched them
        // a moment ago may still be reading them (there are at most a few of them, ever).
        struct Cache
        {
            const char*                  section;
            const char*                  name;      // for the log

            Cache(const char* s, const char* n) : section(s), name(n) {}
            std::atomic<const Speeds*>   current{nullptr};
            std::atomic<uint32_t>        seen{0xFFFFFFFFu};
            std::mutex                   lock;

            const Speeds& Get()
            {
                const uint32_t gen = wxl::ini::Generation();
                if (seen.load(std::memory_order_acquire) != gen || !current.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> g(lock);
                    if (seen.load(std::memory_order_relaxed) != gen || !current.load(std::memory_order_relaxed))
                    {
                        const Speeds* previous = current.load(std::memory_order_relaxed);
                        auto* s = new Speeds(ReadSection(section));
                        bool changed = !previous;
                        for (uint32_t i = 0; previous && i < kMovingTextures; ++i)
                            changed = changed || previous->x[i] != s->x[i] || previous->y[i] != s->y[i] || previous->turn[i] != s->turn[i];
                        if (changed && (previous || s->Any()))
                            wxl::LogCat("SCROLL", "%s: texture 2 (%.2f, %.2f) @ %.2f turns, texture 3 (%.2f, %.2f) @ %.2f, texture 4 (%.2f, %.2f) @ %.2f "
                                        "(lengths and turns per second)", name,
                                        s->x[0], s->y[0], s->turn[0], s->x[1], s->y[1], s->turn[1], s->x[2], s->y[2], s->turn[2]);
                        current.store(s, std::memory_order_release);
                        seen.store(gen, std::memory_order_release);
                    }
                }
                return *current.load(std::memory_order_acquire);
            }
        };

        Cache& ParticleCache() { static Cache c("Particles", "multi-textured particles"); return c; }
        Cache& RibbonCache()   { static Cache c("Ribbons", "multi-textured ribbons"); return c; }
    }

    const Speeds& Particles() { return ParticleCache().Get(); }
    const Speeds& Ribbons() { return RibbonCache().Get(); }

    double Seconds()
    {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<double>(clock::now() - start).count();
    }
}
