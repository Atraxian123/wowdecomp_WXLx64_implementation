// The equip module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "EquipExtension.hpp"

#include "wxl_module.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    // vpathBuildKey is the last member of the engine access added for this module: its presence means all of them are.
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, getService) || !WXL_API_HAS(api, vpathBuildKey)) return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "equip", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    // Subscribes the extension's handlers and installs the hand-item model resolver, exactly as wxl::equip::Init() did
    // when this code was part of the client. Modules load in file-name order, so wxl_equip.dll subscribes before
    // wxl_physics.dll: the equip bone remap and offhand mirror still run before the physics step on OnBuildBonePalette.
    wxl::scripts::equipextension::Create();
    return 0;
}

WXL_MODULE_API void WxlModuleShutdown(void) {}
