// The core's side of the water module: reads the client's state for it and makes the call sites it asks for.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

namespace wxl::api
{
    /// Offers the "wxl.water.v1" service (Engine/Source/WXL/Sdk/wxl_water_api.h). Called by LoadModules before any
    /// module initializes, so the water module finds it however the modules are ordered. 64-bit client only.
    void RegisterWaterHost();
}
