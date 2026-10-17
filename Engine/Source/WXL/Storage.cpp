#include "Engine/Source/WXL/Storage.h"

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

        NativeReadFn g_nativeReader = nullptr;
    }

    void RegisterProvider(ProviderFn fn)
    {
        if (fn) Providers().push_back(fn);
    }

    bool HasProviders()
    {
        return !Providers().empty();
    }

    bool TryServe(const char* name, std::vector<uint8_t>& out)
    {
        if (!name || !*name) return false;
        for (ProviderFn fn : Providers())
        {
            out.clear();
            if (fn(name, out)) return true;
        }
        return false;
    }

    void SetNativeReader(NativeReadFn fn)
    {
        g_nativeReader = fn;
    }

    bool ReadNative(const char* name, std::vector<uint8_t>& out)
    {
        return g_nativeReader && g_nativeReader(name, out);
    }
}
