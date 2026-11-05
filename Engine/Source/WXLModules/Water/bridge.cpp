// See bridge.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "bridge.h"

#include <cstring>

namespace waterbridge
{
    namespace
    {
        const WxlWaterService* g_host = nullptr;

        struct UnitTrampoline
        {
            UnitVisitor visitor;
            void* user;
        };

        void CallVisitor(const WxlWaterUnit* unit, void* user)
        {
            const auto* trampoline = static_cast<const UnitTrampoline*>(user);
            trampoline->visitor(*unit, trampoline->user);
        }
    }

    void Bind(const WxlWaterService* host) { g_host = host; }
    const WxlWaterService* Host() { return g_host; }

    void* D3DDevice() { return g_host ? g_host->d3dDevice() : nullptr; }
    bool CameraInLiquid() { return g_host && g_host->cameraInLiquid() != 0; }

    bool ReadGxMatrices(float view[16], float projection[16])
    {
        return g_host && g_host->readGxMatrices(view, projection) != 0;
    }

    void ReadSceneMatrices(float view[16], float projection[16])
    {
        if (g_host)
            g_host->readSceneMatrices(view, projection);
    }

    bool ReadViewportDepthRange(float& minZ, float& maxZ)
    {
        return g_host && g_host->readViewportDepthRange(&minZ, &maxZ) != 0;
    }

    void ReadCamera(float position[3], float target[3])
    {
        if (g_host)
            g_host->readCamera(position, target);
    }

    float ClientFarClip() { return g_host ? g_host->clientFarClip() : 0.0f; }
    int32_t MapId() { return g_host ? g_host->mapId() : 0; }
    bool ReadLighting(Lighting& out) { return g_host && g_host->readLighting(&out) != 0; }

    bool DescribeLiquid(const void* settings, LiquidType& out)
    {
        out = {};
        return g_host && g_host->describeLiquid(settings, &out) != 0;
    }

    void SettingsBankIdentity(const void*& entries, uint32_t& count)
    {
        entries = nullptr;
        count = 0;
        if (g_host)
            g_host->settingsBankIdentity(&entries, &count);
    }

    bool TransparentLiquidsQueued(const void* liquidRenderer)
    {
        return g_host && g_host->transparentLiquidsQueued(liquidRenderer) != 0;
    }

    void ForEachUnitInLiquid(UnitVisitor visitor, void* user)
    {
        if (!g_host)
            return;
        UnitTrampoline trampoline = {visitor, user};
        g_host->forEachUnitInLiquid(&CallVisitor, &trampoline);
    }

    volatile int32_t* ClientRippleGate() { return g_host ? g_host->clientRippleGate() : nullptr; }
}
