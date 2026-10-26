// The creature module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "CreatureExtension.hpp"

#include "wxl_module.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, getService)) return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "creature", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    // Binds its OnModelLoadPre handler and registers its lazy resolver, exactly as wxl::equip::Init() did.
    wxl::scripts::creatureextension::Create();
    return 0;
}

WXL_MODULE_API void WxlModuleShutdown(void) {}
