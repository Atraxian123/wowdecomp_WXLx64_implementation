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
            s.x[0] = wxl::ini::GetFloat(section, "Texture2XTranslation", 0.0f);
            s.y[0] = wxl::ini::GetFloat(section, "Texture2YTranslation", 0.0f);
            s.x[1] = wxl::ini::GetFloat(section, "Texture3XTranslation", 0.0f);
            s.y[1] = wxl::ini::GetFloat(section, "Texture3YTranslation", 0.0f);
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
                        const bool changed = !previous || previous->x[0] != s->x[0] || previous->y[0] != s->y[0] ||
                                             previous->x[1] != s->x[1] || previous->y[1] != s->y[1];
                        if (changed && (previous || s->Any()))
                            wxl::LogCat("SCROLL", "%s: texture 2 moves (%.2f, %.2f), texture 3 moves (%.2f, %.2f) lengths per second", name,
                                        s->x[0], s->y[0], s->x[1], s->y[1]);
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
