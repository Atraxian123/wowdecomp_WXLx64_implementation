// wxl_water.dll: the modern water module as a WXL module. See water_driver.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "water_driver.h"
#include "wxl_module.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (!api || api->abiVersion != WXL_ABI_VERSION)
        return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "water", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    return wxl::water::Start(api) ? 0 : 1; // 1 = refuse: switched off, or no data, and the log says which
}

WXL_MODULE_API void WxlModuleShutdown(void)
{
    wxl::water::Stop();
}
