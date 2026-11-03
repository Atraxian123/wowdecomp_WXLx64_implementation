// Finds and loads WXL\Modules\*.dll. See ApiHost.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// 64-bit client only: the module API is defined for 64-bit DLLs, and a 32-bit build neither loads nor looks
// for any. Nothing here touches the engine; it needs the log, the ini helpers and the API table.

#include "Engine/Source/WXLApi/ApiHost.h"

#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace wxl::api
{
#if defined(_WIN32) && defined(WOWDECOMP_ARCH_64)

    namespace
    {
        namespace eq = wxl::scripts::equipextension;

        struct Loaded
        {
            ModuleStatus     status;
            HMODULE          dll = nullptr;
            WxlModuleShutdownFn shutdown = nullptr;
        };

        std::mutex           g_lock;
        std::vector<Loaded>  g_modules;
        bool                 g_done = false;

        std::string Narrow(const std::wstring& w)
        {
            if (w.empty()) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::string Lower(std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // The folder the client runs from, with a trailing backslash.
        std::wstring ExeFolder()
        {
            wchar_t path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            std::wstring p(path, n);
            const size_t slash = p.find_last_of(L'\\');
            return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
        }

        // "a.dll;b.dll" / "a.dll,b.dll" -> lower-case names
        std::vector<std::string> SplitList(const char* text)
        {
            std::vector<std::string> out;
            std::string cur;
            for (const char* p = text; ; ++p)
            {
                if (*p == ';' || *p == ',' || *p == '\0')
                {
                    size_t b = 0, e = cur.size();
                    while (b < e && std::isspace(static_cast<unsigned char>(cur[b]))) ++b;
                    while (e > b && std::isspace(static_cast<unsigned char>(cur[e - 1]))) --e;
                    if (e > b) out.push_back(Lower(cur.substr(b, e - b)));
                    cur.clear();
                    if (*p == '\0') break;
                }
                else cur.push_back(*p);
            }
            return out;
        }

        // The DLL's machine type from its PE header, so a 32-bit module is reported as such instead of as
        // "error 193" from LoadLibrary. Returns 0 when the file cannot be read as a PE image.
        uint16_t PeMachine(const std::wstring& path)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return 0;
            uint8_t head[0x40] = {};
            DWORD got = 0;
            uint16_t machine = 0;
            if (ReadFile(f, head, sizeof(head), &got, nullptr) && got == sizeof(head) && head[0] == 'M' && head[1] == 'Z')
            {
                uint32_t peOffset = 0;
                std::memcpy(&peOffset, head + 0x3C, 4);
                uint8_t pe[6] = {};
                if (SetFilePointer(f, static_cast<LONG>(peOffset), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
                    ReadFile(f, pe, sizeof(pe), &got, nullptr) && got == sizeof(pe) && pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)
                    std::memcpy(&machine, pe + 4, 2);
            }
            CloseHandle(f);
            return machine;
        }

        // Structured-exception guards around the three calls into foreign code. Plain functions, no C++
        // objects, so __try is allowed. A module that faults is reported and skipped, not allowed to take
        // the client down on startup.
        bool CallAbi(WxlModuleAbiVersionFn fn, uint32_t* out) noexcept
        {
            __try { *out = fn(); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool CallInit(WxlModuleInitFn fn, const WxlApi* api, WxlModuleInfo* info, int* result) noexcept
        {
            __try { *result = fn(api, info); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        void CallShutdown(WxlModuleShutdownFn fn) noexcept
        {
            __try { fn(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void Skip(Loaded& m, const char* why)
        {
            m.status.loaded = false;
            m.status.note = why;
            wxl::LogCat("MODULE", "%s: skipped, %s", m.status.file.c_str(), why);
        }

        void LoadOne(const std::wstring& path, const std::string& file, Loaded& m)
        {
            m.status.file = file;

            const uint16_t machine = PeMachine(path);
            if (machine == 0) { Skip(m, "not a readable DLL"); return; }
            if (machine != IMAGE_FILE_MACHINE_AMD64) { Skip(m, "not a 64-bit DLL (modules are for the 64-bit client)"); return; }

            m.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m.dll)
            {
                char why[96];
                std::snprintf(why, sizeof(why), "LoadLibrary failed (error %lu: a missing dependency?)", GetLastError());
                Skip(m, why);
                return;
            }

            auto abiFn  = reinterpret_cast<WxlModuleAbiVersionFn>(GetProcAddress(m.dll, "WxlModuleAbiVersion"));
            auto initFn = reinterpret_cast<WxlModuleInitFn>(GetProcAddress(m.dll, "WxlModuleInit"));
            m.shutdown  = reinterpret_cast<WxlModuleShutdownFn>(GetProcAddress(m.dll, "WxlModuleShutdown"));
            if (!abiFn || !initFn)
            {
                Skip(m, "does not export WxlModuleAbiVersion / WxlModuleInit: not a WXL module");
                FreeLibrary(m.dll);
                m.dll = nullptr;
                return;
            }

            uint32_t abi = 0;
            if (!CallAbi(abiFn, &abi)) { Skip(m, "faulted in WxlModuleAbiVersion"); FreeLibrary(m.dll); m.dll = nullptr; return; }
            if (abi != WXL_ABI_VERSION)
            {
                char why[96];
                std::snprintf(why, sizeof(why), "built for module ABI %u, this client speaks %u", abi, WXL_ABI_VERSION);
                Skip(m, why);
                FreeLibrary(m.dll);
                m.dll = nullptr;
                return;
            }

            WxlModuleInfo info = {};
            info.structSize = sizeof(info);
            int result = -1;
            if (!CallInit(initFn, Table(), &info, &result))
            {
                // a fault inside the module's init: the module may be half set up, so it stays loaded and is never called again
                Skip(m, "faulted in WxlModuleInit");
                m.shutdown = nullptr;
                return;
            }
            if (result != 0)
            {
                char why[96];
                std::snprintf(why, sizeof(why), "WxlModuleInit refused (returned %d)", result);
                Skip(m, why);
                FreeLibrary(m.dll);
                m.dll = nullptr;
                m.shutdown = nullptr;
                return;
            }

            info.name[sizeof(info.name) - 1] = '\0';
            m.status.name = info.name;
            m.status.major = info.versionMajor;
            m.status.minor = info.versionMinor;
            m.status.loaded = true;
            m.status.note = "ok";
            wxl::LogCat("MODULE", "%s: loaded '%s' %u.%u", file.c_str(), info.name[0] ? info.name : "(unnamed)", info.versionMajor, info.versionMinor);
        }
    }

    void LoadModules()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_done) return;
        g_done = true;

        if (!eq::WxlIniGetBool("Modules", "Enabled", true))
        {
            wxl::LogCat("MODULE", "modules are switched off ([Modules] Enabled=0 in WXLExtendedEquipment.ini)");
            return;
        }

        const std::wstring folder = ExeFolder() + L"WXL\\Modules\\";
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((folder + L"*.dll").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            wxl::LogCat("MODULE", "no modules (no *.dll in %s)", Narrow(folder).c_str());
            return;
        }

        std::vector<std::wstring> names;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            return Lower(Narrow(a)) < Lower(Narrow(b));
        });

        char disableText[512] = {};
        eq::WxlIniGetString("Modules", "Disable", disableText, sizeof(disableText));
        const std::vector<std::string> disabled = SplitList(disableText);

        wxl::LogCat("MODULE", "module API %u, looking in %s (%zu file%s)", WXL_ABI_VERSION, Narrow(folder).c_str(),
                    names.size(), names.size() == 1 ? "" : "s");
        g_modules.reserve(names.size());
        for (const std::wstring& name : names)
        {
            g_modules.emplace_back();
            Loaded& m = g_modules.back();
            const std::string file = Narrow(name);
            if (std::find(disabled.begin(), disabled.end(), Lower(file)) != disabled.end())
            {
                m.status.file = file;
                Skip(m, "listed in [Modules] Disable");
                continue;
            }
            LoadOne(folder + name, file, m);
        }
    }

    void UnloadModules()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it)
        {
            if (it->shutdown) CallShutdown(it->shutdown);
            // the DLL stays mapped: its event handlers are still in the bus, and the process is going away
            it->shutdown = nullptr;
        }
    }

    std::vector<ModuleStatus> Modules()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        std::vector<ModuleStatus> out;
        for (const Loaded& m : g_modules) out.push_back(m.status);
        return out;
    }

#else   // 32-bit build, or not Windows: the module API is 64-bit only

    void LoadModules()
    {
        wxl::LogCat("MODULE", "the module API is for the 64-bit client: no modules are loaded in this build");
    }
    void UnloadModules() {}
    std::vector<ModuleStatus> Modules() { return {}; }

#endif
}
