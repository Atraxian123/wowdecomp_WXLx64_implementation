// The module's view of the client: the "wxl.water.v1" service (Engine/Source/WXL/Sdk/wxl_water_api.h) behind the
// names the rest of the module was written against. The module includes no engine header; the core reads the
// client's own objects for it (Engine/Source/WXLApi/WaterHost.cpp).
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)
#pragma once

#include "wxl_water_api.h"

#include <cstdint>

namespace waterbridge
{
    using Lighting = WxlWaterLighting;
    using LiquidType = WxlWaterLiquidType;
    using WaterUnit = WxlWaterUnit;
    using UnitVisitor = void (*)(const WaterUnit& unit, void* user);

    /// Binds the service the core registered. Null leaves every read below answering "nothing".
    void Bind(const WxlWaterService* host);
    const WxlWaterService* Host();

    void* D3DDevice();
    bool CameraInLiquid();
    bool ReadGxMatrices(float view[16], float projection[16]);
    void ReadSceneMatrices(float view[16], float projection[16]);
    bool ReadViewportDepthRange(float& minZ, float& maxZ);
    void ReadCamera(float position[3], float target[3]);
    float ClientFarClip();
    int32_t MapId();
    bool ReadLighting(Lighting& out);
    bool DescribeLiquid(const void* settings, LiquidType& out);
    void SettingsBankIdentity(const void*& entries, uint32_t& count);
    bool TransparentLiquidsQueued(const void* liquidRenderer);
    void ForEachUnitInLiquid(UnitVisitor visitor, void* user);
    volatile int32_t* ClientRippleGate();
}
