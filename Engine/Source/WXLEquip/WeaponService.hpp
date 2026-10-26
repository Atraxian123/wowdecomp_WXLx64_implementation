// The equip extension's view of the weapon module (a DLL, WXL\Modules\wxl_weapon.dll).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The weapon module decides which model a weapon gets and bakes it; the equip extension, which attaches the
// model to a hand, asks it through the "weapon" service (WxlWeaponService, Sdk/wxl_api.h). The functions keep
// the names and signatures they had when the weapon code was compiled into the client. When the module is not
// loaded they answer false: the equip extension then leaves the weapon to the native handling.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::scripts::weaponextension
{
    /// The virtual path of a baked weapon model for (displayId, column); false when there is none.
    bool WeaponGetVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz);

    /// Same for the off hand; *outAutoMirrored tells whether the baked bytes were mirrored geometrically.
    bool WeaponGetOffhandVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz,
                                     bool* outAutoMirrored = nullptr);

    /// [Weapons] GeometricMirror: whether off-hand models are mirrored in their bytes (then the render-time mirror must not run).
    bool WeaponMirrorIsGeometric();
}
