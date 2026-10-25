// The physics module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "PhysicsLive.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (info)
    {
        std::strncpy(info->name, "physics", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    return wxl::equip::physics::Init(api) ? 0 : 1;
}

WXL_MODULE_API void WxlModuleShutdown(void)
{
    // the event handler stays subscribed until the process ends; make it a no-op, and drop every instance
    wxl::equip::physics::SetEnabled(false);
}
