// See WeaponService.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "WeaponService.hpp"

#include "wxl_module.hpp"

#include <atomic>

namespace wxl::scripts::weaponextension
{
    namespace
    {
        // Found on first use and cached: the weapon module registers its service when it loads, which can be after
        // this module started (wxl_weapon.dll sorts after wxl_equip.dll), and never changes afterwards.
        const WxlWeaponService* Service()
        {
            static std::atomic<const WxlWeaponService*> s_cached{ nullptr };
            const WxlWeaponService* s = s_cached.load(std::memory_order_acquire);
            if (s) return s;
            size_t size = 0;
            const WxlApi* api = wxl::module::Api();
            s = api && api->getService ? static_cast<const WxlWeaponService*>(api->getService(WXL_SERVICE_WEAPON, &size)) : nullptr;
            if (s && size >= sizeof(WxlWeaponService) && s->structSize >= sizeof(WxlWeaponService))
                s_cached.store(s, std::memory_order_release);
            else
                s = nullptr;
            return s;
        }
    }

    bool WeaponGetVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz)
    {
        const WxlWeaponService* s = Service();
        return s && s->getVirtualPath && s->getVirtualPath(displayId, column, out, outSz) != 0;
    }

    bool WeaponGetOffhandVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz, bool* outAutoMirrored)
    {
        if (outAutoMirrored) *outAutoMirrored = false;
        const WxlWeaponService* s = Service();
        if (!s || !s->getOffhandVirtualPath) return false;
        int mirrored = 0;
        const bool ok = s->getOffhandVirtualPath(displayId, column, out, outSz, &mirrored) != 0;
        if (outAutoMirrored) *outAutoMirrored = mirrored != 0;
        return ok;
    }

    bool WeaponMirrorIsGeometric()
    {
        const WxlWeaponService* s = Service();
        return s && s->mirrorIsGeometric && s->mirrorIsGeometric() != 0;
    }
}
