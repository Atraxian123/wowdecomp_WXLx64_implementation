// See ModuleRegistry.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Engine/Source/WXL/ModuleRegistry.h"

#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Storage.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace wxl::registry
{
    namespace
    {
        constexpr uint32_t kMaxEntries = 16;

        // A fixed array whose entries are written before the count that publishes them, so a loader thread
        // that is already inside a file open reads it without a lock (providers run on every open).
        template <class Fn>
        struct Slots
        {
            struct Entry { Fn fn = nullptr; void* user = nullptr; };
            Entry                 entries[kMaxEntries];
            std::atomic<uint32_t> count{ 0 };
            std::mutex            writeLock;

            int Add(Fn fn, void* user)
            {
                if (!fn) return 1;
                std::lock_guard<std::mutex> lock(writeLock);
                const uint32_t n = count.load(std::memory_order_relaxed);
                if (n >= kMaxEntries) return 2;
                entries[n] = Entry{ fn, user };
                count.store(n + 1, std::memory_order_release);
                return 0;
            }
        };

        Slots<WxlFileProviderFn>& Providers() { static Slots<WxlFileProviderFn> s; return s; }
        Slots<WxlVirtualKeyFn>&   Queries()   { static Slots<WxlVirtualKeyFn> s;   return s; }

        // The core's side of WxlServeSink: grow the vector the storage layer handed us.
        void* Reserve(void* ctx, size_t size)
        {
            try
            {
                auto* out = static_cast<std::vector<uint8_t>*>(ctx);
                out->resize(size);
                return out->data();
            }
            catch (...) { return nullptr; }
        }

        // The one provider the storage layer knows about. It asks every module provider in turn.
        bool Dispatch(const char* name, std::vector<uint8_t>& out)
        {
            auto& slots = Providers();
            const uint32_t n = slots.count.load(std::memory_order_acquire);
            const WxlServeSink sink = { &out, &Reserve };
            for (uint32_t i = 0; i < n; ++i)
            {
                out.clear();
                if (slots.entries[i].fn(slots.entries[i].user, name, &sink) && !out.empty()) return true;
                out.clear();   // a provider that said no (or served nothing) leaves no half-filled buffer behind
            }
            return false;
        }
    }

    int RegisterFileProvider(WxlFileProviderFn fn, void* user)
    {
        const int rc = Providers().Add(fn, user);
        if (rc != 0) return rc;

        static std::once_flag s_installed;
        std::call_once(s_installed, [] { wxl::storage::RegisterProvider(&Dispatch); });
        wxl::LogCat("MODULE", "file provider registered (module providers: %u)", Providers().count.load());
        return 0;
    }

    int RegisterVirtualKeyQuery(WxlVirtualKeyFn fn, void* user)
    {
        return Queries().Add(fn, user);
    }

    int IsVirtualKey(const char* name)
    {
        if (!name || !*name) return 0;
        auto& slots = Queries();
        const uint32_t n = slots.count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (slots.entries[i].fn(slots.entries[i].user, name)) return 1;
        return 0;
    }
}
