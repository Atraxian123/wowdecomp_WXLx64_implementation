// See ModuleHost.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "ModuleHost.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX   // windows.h must not turn std::min / std::max into macros
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wxl::modules::host
{
    namespace { const WxlApi* g_api = nullptr; }

    void SetApi(const WxlApi* api) { g_api = api; }
    const WxlApi* Api() { return g_api; }

    void Log(const char* line)
    {
        if (g_api && g_api->log) g_api->log("LISTFILE", line ? line : "");
    }

    bool IniGetBool(const char* section, const char* key, bool defaultValue)
    {
        return g_api && g_api->iniGetBool ? g_api->iniGetBool(section, key, defaultValue ? 1 : 0) != 0 : defaultValue;
    }

    std::string IniGetString(const char* section, const char* key, const char* defaultValue)
    {
        char buf[520] = {};
        if (g_api && g_api->iniGetString && g_api->iniGetString(section, key, buf, sizeof(buf)) && buf[0]) return buf;
        return defaultValue ? defaultValue : "";
    }

    bool FileExists(const char* name)
    {
        return g_api && g_api->fileExists && g_api->fileExists(name) != 0;
    }

    std::string ClientFolder()
    {
#if defined(_WIN32)
        wchar_t w[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, w, static_cast<DWORD>(sizeof(w) / sizeof(w[0])));
        if (n == 0 || n >= sizeof(w) / sizeof(w[0])) return std::string();
        std::wstring p(w, n);
        const size_t slash = p.find_last_of(L'\\');
        if (slash == std::wstring::npos) return std::string();
        p.resize(slash + 1);
        const int len = WideCharToMultiByte(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), &s[0], len, nullptr, nullptr);
        return s;
#else
        char buf[4096] = {};
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string();
        std::string p(buf, static_cast<size_t>(n));
        const size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
#endif
    }
}
