// The one place the modern water module touches the decompiled client's own data.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)
//
// The standalone module read the 3.3.5a client through absolute addresses. In the decompiled client every
// one of those values is a named object, so this file reads them by name and hands the rest of the module plain
// structs. Only bridge.cpp includes engine headers; everything else in this library sees nothing but this header.
#pragma once

#include <cstdint>

namespace waterbridge
{
    /// The client's live IDirect3DDevice9, or null before the Gx device exists / when it is not a D3D9 device.
    void* D3DDevice();

    /// World::SceneCamLiquidStatus: the camera is inside a liquid.
    bool CameraInLiquid();

    /// The Gx view-stack top (camera-relative view) and the Gx projection, 16 floats each.
    bool ReadGxMatrices(float view[16], float projection[16]);

    /// The world scene's own view / projection globals, for when the Gx stack is not usable.
    void ReadSceneMatrices(float view[16], float projection[16]);

    /// The depth range of the current Gx viewport.
    bool ReadViewportDepthRange(float& minZ, float& maxZ);

    void ReadCamera(float position[3], float target[3]);
    float ClientFarClip();
    int32_t MapId();

    /// Day / night lighting and the water colours the client has interpolated for the camera's position.
    struct Lighting
    {
        float dayFraction;
        float sunPosition[3];
        float moonPosition[3];
        float skyCenter[3];
        float sunDayStart;
        float sunDayEnd;
        uint32_t fogColor;
        float fogStart[2];
        float fogEnd[2];
        uint32_t sunColor;
        uint32_t directColor;
        uint32_t ambientColor;
        float zoneFogDistance;
        float stormBlend;
        int32_t screenEffectLightSlot;
        uint32_t skyColors[6];
        uint32_t riverColors[2];
        uint32_t oceanColors[2];
    };
    bool ReadLighting(Lighting& out);

    /// What the client's liquid type tables say about one liquid material settings object.
    struct LiquidType
    {
        bool inBank;     // the settings object is one of the client's per-liquid-type settings
        bool hasRow;     // and its LiquidType row exists
        uint32_t id;
        uint32_t flags;
        uint32_t soundBank;
        uint32_t materialId;
        char texture0[128];
    };
    bool DescribeLiquid(const void* settings, LiquidType& out);

    /// Identity of the settings table, so a reload invalidates anything cached against it.
    void SettingsBankIdentity(const void*& entries, uint32_t& count);

    /// Liquid::CRenderer: transparent (pass 1) liquids are queued.
    bool TransparentLiquidsQueued(const void* liquidRenderer);

    /// A visible unit that stands in liquid deep enough to cross its body.
    struct WaterUnit
    {
        uint64_t guid;
        float position[3];
        float surface;
        float radius;
        float height;
        bool swimming;
        bool onTransport;
    };
    using UnitVisitor = void (*)(const WaterUnit& unit, void* user);
    void ForEachUnitInLiquid(UnitVisitor visitor, void* user);

    /// CWorld::s_waterRipples: the client's own splash and wake sprites switch.
    volatile int32_t* ClientRippleGate();
}
