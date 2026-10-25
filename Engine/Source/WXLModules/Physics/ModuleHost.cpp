// See ModuleHost.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "ModuleHost.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wxl::modules::host
{
    namespace { const WxlApi* g_api = nullptr; }

    void SetApi(const WxlApi* api) { g_api = api; }
    const WxlApi* Api() { return g_api; }

    bool IniGetBool(const char* section, const char* key, bool defaultValue)
    {
        return g_api && g_api->iniGetBool ? g_api->iniGetBool(section, key, defaultValue ? 1 : 0) != 0 : defaultValue;
    }
    int IniGetInt(const char* section, const char* key, int defaultValue)
    {
        return g_api && g_api->iniGetInt ? g_api->iniGetInt(section, key, defaultValue) : defaultValue;
    }
    bool IniGetString(const char* section, const char* key, char* out, size_t cap)
    {
        if (out && cap) out[0] = '\0';
        return g_api && g_api->iniGetString && g_api->iniGetString(section, key, out, cap) != 0;
    }
    uint64_t IniStamp() { return g_api && g_api->iniStamp ? g_api->iniStamp() : 0; }

    bool ReadFile(const char* name, std::vector<uint8_t>& out)
    {
        if (!g_api || !g_api->readFile || !g_api->freeBuffer) return false;
        WxlBuffer buf = {};
        if (!g_api->readFile(name, &buf) || !buf.data) return false;
        out.assign(static_cast<const uint8_t*>(buf.data), static_cast<const uint8_t*>(buf.data) + buf.size);
        g_api->freeBuffer(&buf);   // freed by the side that allocated it
        return true;
    }

    bool IsVirtualKey(const char* name)
    {
        return g_api && g_api->isVirtualKey && g_api->isVirtualKey(name) != 0;
    }
}

namespace wxl
{
    void LogCat(const char* category, const char* format, ...)
    {
        const WxlApi* api = modules::host::Api();
        if (!api || !api->log) return;
        char line[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        api->log(category, line);
    }

    bool LogThrottle(const char* key, unsigned first, unsigned every)
    {
        const WxlApi* api = modules::host::Api();
        return api && api->logThrottle && api->logThrottle(key, first, every) != 0;
    }
}
