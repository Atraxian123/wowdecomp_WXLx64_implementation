// The core's side of the module API (Engine/Source/WXL/Sdk/wxl_api.h).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wxl::api
{
    /// The table every module receives. Static, valid for the life of the process.
    const WxlApi* Table();

    /**
     * @brief Loads every DLL in <client folder>\WXL\Modules (64-bit client only; a no-op on Win32).
     *
     * Call once, after wxl::equip::Init(): modules subscribe to events, subscribers run in subscription order,
     * and a module that wants to see the palette after the built-in extensions have changed it must subscribe
     * after them. DLLs load in file-name order (prefix a name with 10_, 20_ ... to order them).
     *
     * [Modules] in WXLExtendedEquipment.ini:  Enabled=1 (default), Disable=a.dll;b.dll
     * A DLL that is not a 64-bit module, has the wrong ABI, or refuses to initialize is skipped and logged.
     */
    void LoadModules();

    /// Calls every loaded module's WxlModuleShutdown (when it has one), in reverse order, then frees them.
    void UnloadModules();

    struct ModuleStatus
    {
        std::string file;      ///< file name
        std::string name;      ///< what the module called itself ("" until it initialized)
        uint32_t    major = 0, minor = 0;
        bool        loaded = false;
        std::string note;      ///< why it was skipped, or "ok"
    };
    /// What LoadModules found, in load order.
    std::vector<ModuleStatus> Modules();
}
