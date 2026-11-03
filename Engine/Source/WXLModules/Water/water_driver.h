// Drives the modern water module from the client's world render.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)
//
// Opt-in: wxl_water.dll starts only with
//
//     [Water]
//     Enabled=1
//
// in WXLExtendedEquipment.ini (beside the client executable). Its own settings, waterdata.bin and log are in
// WXL\Modules\wxl_water\ . This file is the standalone module's hooks.cpp, water half, without the byte patching:
// the core calls the moments registered through the water service at the same points the module used to detour.
#pragma once

#include "wxl_api.h"

namespace wxl::water
{
    /// Loads the settings and data, registers the module's moments with the core, adopts the device. False refuses
    /// the module (switched off, or the data is missing); the reason is in the log.
    bool Start(const WxlApi* api);

    /// Withdraws the module's moments and gives the client's device back. Called at exit.
    void Stop();
}
