// Drives the modern water module from the decompiled client's world render.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)
//
// OPT-IN. Off unless WXLExtendedEquipment.ini (beside the client executable) says:
//
//     [Water]
//     Enabled=1
//
// The module's own settings live in WXLWater\wxl-modern-water.ini beside it, with waterdata.bin (the water
// presets and FFT tiles). Both are read once, at startup.
//
// This file is the standalone module's hooks.cpp, water half, without the byte patching: the client calls
// wxl::waterhooks at the same points the module used to detour, and the device is adopted rather than
// wrapped at creation.
#pragma once

namespace wxl::water
{
    /// Reads the ini and, when [Water] Enabled=1, starts the module. Call once at startup after wxl::InitModules().
    void Init();
}
