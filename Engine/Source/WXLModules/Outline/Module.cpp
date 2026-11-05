// The outline module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "wxl_module.hpp"

#include <cstring>

namespace wxl::outline
{
    bool Start();   // Outline.cpp: reads [Outline] and subscribes when Enabled=1 or Quest=1
}

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    // outlineBatchState is the last member of the engine access added for this module: its presence means all of them
    // are (and modelInfo, from the equip module's section, which the target walk uses).
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, outlineBatchState)) return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "outline", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    // Opt-in: with [Outline] Enabled and Quest both 0 or absent the module declines and is unloaded again, so it
    // costs nothing then.
    return wxl::outline::Start() ? 0 : 1;
}

WXL_MODULE_API void WxlModuleShutdown(void) {}
