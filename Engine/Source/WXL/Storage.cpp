#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXL/Log.h"

#include <atomic>
#include <vector>

namespace wxl::storage
{
    namespace
    {
        std::vector<ProviderFn>& Providers()
        {
            static std::vector<ProviderFn> s_providers;
            return s_providers;
        }

        NativeReadFn   g_nativeReader = nullptr;
        NativeExistsFn g_nativeExists = nullptr;
    }

    void RegisterProvider(ProviderFn fn)
    {
        if (!fn) return;
        Providers().push_back(fn);
        wxl::LogCat("STORAGE", "provider registered (total=%zu)", Providers().size());
    }

    bool HasProviders()
    {
        return !Providers().empty();
    }

    namespace
    {
        std::atomic<uint32_t> g_opens{ 0 };
        std::atomic<uint32_t> g_hits{ 0 };
    }

    bool TryServe(const char* name, std::vector<uint8_t>& out)
    {
        if (!name || !*name) return false;

        const uint32_t opens = ++g_opens;
        for (ProviderFn fn : Providers())
        {
            out.clear();
            if (fn(name, out))
            {
                const uint32_t hits = ++g_hits;
                if (wxl::LogThrottle("storage.hit", 400, 100))
                    wxl::LogCat("STORAGE", "SERVED virtual file '%s' (%zu bytes) [hit #%u of %u opens]",
                                name, out.size(), hits, opens);
                return true;
            }
        }

        if (opens % 20000 == 0)
            wxl::LogCat("STORAGE", "stats: %u opens offered to providers, %u served virtually", opens, g_hits.load());
        return false;
    }

    void SetNativeReader(NativeReadFn fn)
    {
        g_nativeReader = fn;
        wxl::LogCat("STORAGE", "native reader %s", fn ? "installed" : "cleared");
    }

    void SetNativeExists(NativeExistsFn fn)
    {
        g_nativeExists = fn;
    }

    bool ExistsNative(const char* name)
    {
        return g_nativeExists && name && *name && g_nativeExists(name);
    }

    bool ReadNativeQuiet(const char* name, std::vector<uint8_t>& out)
    {
        return g_nativeReader && name && g_nativeReader(name, out);
    }

    bool ReadNative(const char* name, std::vector<uint8_t>& out)
    {
        if (!g_nativeReader)
        {
            if (wxl::LogThrottle("storage.noreader", 5, 0))
                wxl::LogCat("STORAGE", "ReadNative('%s') FAILED: no native reader installed", name ? name : "(null)");
            return false;
        }
        const bool ok = g_nativeReader(name, out);
        if (!ok && wxl::LogThrottle("storage.readfail", 60, 200))
            wxl::LogCat("STORAGE", "ReadNative('%s') -> not found or unreadable", name ? name : "(null)");
        return ok;
    }
}
