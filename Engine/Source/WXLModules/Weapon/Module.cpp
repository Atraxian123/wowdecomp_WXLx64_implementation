// The weapon module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "WeaponExtension.hpp"

#include "wxl_module.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

namespace
{
    // The equip extension asks the weapon module what to attach to a hand; it reaches these through the
    // "weapon" service (WxlWeaponService in wxl_api.h), by name, whenever it needs them.
    int ServiceGetVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz)
    {
        return wxl::scripts::weaponextension::WeaponGetVirtualPath(displayId, column, out, outSz) ? 1 : 0;
    }
    int ServiceGetOffhandVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz, int* outAutoMirrored)
    {
        bool mirrored = false;
        const bool ok = wxl::scripts::weaponextension::WeaponGetOffhandVirtualPath(displayId, column, out, outSz, &mirrored);
        if (outAutoMirrored) *outAutoMirrored = mirrored ? 1 : 0;
        return ok ? 1 : 0;
    }
    int ServiceMirrorIsGeometric() { return wxl::scripts::weaponextension::WeaponMirrorIsGeometric() ? 1 : 0; }

    const WxlWeaponService g_service = { sizeof(WxlWeaponService), &ServiceGetVirtualPath, &ServiceGetOffhandVirtualPath,
                                         &ServiceMirrorIsGeometric };
}

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, getService)) return 1;
    wxl::module::Bind(api);
    if (info)
    {
        std::strncpy(info->name, "weapon", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }
    if (api->registerService(WXL_SERVICE_WEAPON, &g_service, sizeof(g_service)) != 0) return 2;
    wxl::scripts::weaponextension::Create();
    return 0;
}

WXL_MODULE_API void WxlModuleShutdown(void) {}
