// The listfile module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h and wxl_listfile_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
//   WXLExtendedEquipment.ini
//     [Listfile]
//     Eager=1                       1 = read listfile.csv once, at load, into a lookup table (default)
//                                   0 = leave it on disk: each lookup reads the file (answers are cached)
//     Path=WXL\listfile.csv         relative to the client folder, or absolute
//     CheckExists=1                 1 = also ask the client whether the file the listfile names is really there
//     NotFoundName=WXL\NotFound     what a lookup answers for a file that is not found (+ .blp / .m2 by kind): a name
//                                   the client cannot open, so it falls back to its own missing texture / model

#include "ModuleHost.hpp"
#include "Resolver.hpp"

#include "wxl_listfile_api.h"

#include <cstdio>
#include <cstring>
#include <vector>

WXL_DEFINE_MODULE_ABI()

namespace
{
    namespace lf = wxl::listfile;
    namespace host = wxl::modules::host;

    lf::Table    g_table(&host::Log);
    lf::Resolver g_resolver;
    bool         g_eager = true;

    // ---- the service: the C-ABI face of the resolver. No exception leaves the module. ----

    int SvcLookupPath(uint32_t id, char* out, size_t cap)
    {
        try { return g_resolver.LookupPath(id, out, cap); } catch (...) { return -1; }
    }
    int SvcResolve(uint32_t id, uint32_t kind, char* out, size_t cap)
    {
        try { return g_resolver.Resolve(id, kind, out, cap); } catch (...) { return WXL_LISTFILE_ERROR; }
    }
    int SvcResolvePath(const char* path, uint32_t kind, char* out, size_t cap)
    {
        try { return g_resolver.ResolvePath(path, kind, out, cap); } catch (...) { return WXL_LISTFILE_ERROR; }
    }
    uint32_t SvcEntryCount() { return g_table.EntryCount(); }
    int      SvcIsEager()    { return g_eager ? 1 : 0; }
    int      SvcReload()
    {
        try { return g_table.Reload() ? 1 : 0; } catch (...) { return 0; }
    }

    // Static, valid for the life of the process (the client and other modules keep the pointer).
    const WxlListfileServices g_services = {
        sizeof(WxlListfileServices),
        &SvcLookupPath,
        &SvcResolve,
        &SvcResolvePath,
        &SvcEntryCount,
        &SvcIsEager,
        &SvcReload,
    };

    // The ini spells paths the Windows way. The client is a Windows program, where this changes nothing; elsewhere
    // (the off-Windows module test) a backslash would be a character of the file name, so turn them into '/'.
    std::string NativeSeparators(std::string p)
    {
#if !defined(_WIN32)
        for (char& c : p) if (c == '\\') c = '/';
#endif
        return p;
    }

    bool IsAbsolute(const std::string& p)
    {
        return (p.size() >= 2 && p[1] == ':') || (!p.empty() && (p[0] == '\\' || p[0] == '/'));
    }
}

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (info)
    {
        std::strncpy(info->name, "listfile", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }

    // fileExists is the newest member this module uses; it comes after the service ones.
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, fileExists) || !api->log || !api->iniGetBool || !api->iniGetString)
        return 1;
    host::SetApi(api);

    g_eager                 = host::IniGetBool("Listfile", "Eager", true);
    const bool checkExists  = host::IniGetBool("Listfile", "CheckExists", true);
    std::string path        = host::IniGetString("Listfile", "Path", "WXL\\listfile.csv");
    const std::string notFound = host::IniGetString("Listfile", "NotFoundName", "WXL\\NotFound");
    path = NativeSeparators(path);
    if (!IsAbsolute(path)) path = host::ClientFolder() + path;

    lf::Hooks hooks;
    hooks.exists = &host::FileExists;
    hooks.log    = &host::Log;
    g_resolver.Configure(&g_table, hooks, checkExists, notFound);

    // A missing listfile is not a reason to refuse: the module still loads and answers "not found".
    g_table.Configure(path, g_eager);

    if (api->registerService(WXL_SERVICE_LISTFILE, &g_services, sizeof(g_services)) != 0)
    {
        host::Log("the service " WXL_SERVICE_LISTFILE " is already registered");
        return 3;
    }

    char line[640];
    std::snprintf(line, sizeof(line), "listfile module ready: %s, '%s', existence check %s",
                  g_eager ? "eager table" : "read at run time", path.c_str(), checkExists ? "on" : "off");
    host::Log(line);
    return 0;
}

WXL_MODULE_API void WxlModuleShutdown(void)
{
    // Nothing to tear down: the table is plain memory.
    host::Log("listfile module shutdown");
}
