// See WeaponService.hpp.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "Engine/Source/WXLEquip/WeaponService.hpp"

#include "Engine/Source/WXL/Services.h"

#define WXL_ALLOW_32BIT_HEADER 1   // host side: only the struct declaration is needed, no module is built here
#include "Engine/Source/WXL/Sdk/wxl_api.h"

#include <atomic>

namespace wxl::scripts::weaponextension
{
    namespace
    {
        // Found on first use and cached: the module registers itself when it loads, which can be after
        // the equip extension was created, and never changes afterwards.
        const WxlWeaponService* Service()
        {
            static std::atomic<const WxlWeaponService*> s_cached{ nullptr };
            const WxlWeaponService* s = s_cached.load(std::memory_order_acquire);
            if (s) return s;
            size_t size = 0;
            s = static_cast<const WxlWeaponService*>(wxl::services::Get(WXL_SERVICE_WEAPON, &size));
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
