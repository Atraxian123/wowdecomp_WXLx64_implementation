// Reads the one WXLSetup.ini switch the M2 draw context depends on. See DrawContext.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "Engine/Source/WXL/DrawContext.h"

#include "Engine/Source/WXL/Ini.h"

#include <atomic>

namespace wxl::events
{
    bool BlendedM2BatchesWanted()
    {
        // Looked at again whenever the ini changes (a save, or a console override). The check is an atomic load.
        static std::atomic<uint32_t> seen{0xFFFFFFFFu};
        static std::atomic<bool> wanted{false};
        const uint32_t gen = wxl::ini::Generation();
        if (seen.load(std::memory_order_acquire) != gen)
        {
            wanted.store(wxl::ini::GetInt("Outline", "TransparentUnits", 0) != 0, std::memory_order_relaxed);
            seen.store(gen, std::memory_order_release);
        }
        return wanted.load(std::memory_order_relaxed);
    }
}
