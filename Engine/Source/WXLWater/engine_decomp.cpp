// engine:: for the decompiled client. Replaces the standalone module's engine.cpp, which read the 3.3.5a
// client through absolute addresses: every value here comes from waterbridge (bridge.h) instead, and the
// address and code-byte guards that only made sense for a binary patch are gone, since the client is built
// from the same source as this module.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "engine.h"

#include "bridge.h"
#include "log.h"
#include "water_classify.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine
{
namespace
{
constexpr float kMinWorldDepthRangeSpan = 0.01f;

struct OpaqueState
{
    bool valid;
    D3DVIEWPORT9 viewport;
    float cameraRelativeView[16];
    float glProjection[16];
};

OpaqueState g_opaque = {};

bool Finite(const float* v, int count)
{
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(v[i]))
            return false;
    return true;
}

bool IsPerspective(const float* p)
{
    return Finite(p, 16) && std::fabs(p[11] - 1.0f) < 1e-3f && std::fabs(p[15]) < 1e-3f &&
           std::fabs(p[0]) > 1e-4f && std::fabs(p[5]) > 1e-4f;
}

bool IsWorldDepthRange(float minZ, float maxZ)
{
    return std::isfinite(minZ) && std::isfinite(maxZ) && minZ >= 0.0f && maxZ <= 1.0f &&
           maxZ - minZ > kMinWorldDepthRangeSpan;
}

// The viewport the device reports is the one last sent to the hardware; the Gx layer may still hold a newer
// depth range it has not flushed yet, and the world's range is the one the depth buffer was written with.
void ApplyPendingGxViewportDepthRange(D3DVIEWPORT9& viewport)
{
    float minZ = 0.0f, maxZ = 0.0f;
    if (waterbridge::ReadViewportDepthRange(minZ, maxZ) && IsWorldDepthRange(minZ, maxZ))
    {
        viewport.MinZ = minZ;
        viewport.MaxZ = maxZ;
    }
}

bool UsableOpaqueState(const OpaqueState& state)
{
    return state.viewport.Width > 0 && state.viewport.Height > 0 && IsPerspective(state.glProjection) &&
           Finite(state.cameraRelativeView, 16);
}

void Normalize(float* v)
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1e-6f)
        for (int i = 0; i < 3; ++i)
            v[i] /= length;
}

LightParamsSelection SelectLightParams(const waterbridge::Lighting& light)
{
    LightParamsSelection selection;
    selection.stormBlend = std::isfinite(light.stormBlend) ? std::clamp(light.stormBlend, 0.0f, 1.0f) : 0.0f;
    if (light.screenEffectLightSlot >= 0 && light.screenEffectLightSlot < FogData::kLightParamsSlots)
        selection.screenEffectSlot = light.screenEffectLightSlot;
    return selection;
}

// ---- liquid classification ----

constexpr int kClassifiedSettingsCacheSize = 32;
constexpr int kMaxLoggedLiquidTypes = 64;

struct ClassifiedSettings
{
    const void* settings;
    WaterClass waterClass;
};

struct ClassifiedSettingsCache
{
    const void* bankEntries;
    uint32_t bankCount;
    int count;
    int next;
    ClassifiedSettings entries[kClassifiedSettingsCacheSize];
};

ClassifiedSettingsCache g_classified = {};
uint32_t g_loggedLiquidTypes[kMaxLoggedLiquidTypes] = {};
int g_loggedLiquidTypeCount = 0;
bool g_loggedUnreadableSettings = false;

const ClassifiedSettings* FindClassified(const void* settings)
{
    for (int i = 0; i < g_classified.count; ++i)
        if (g_classified.entries[i].settings == settings)
            return &g_classified.entries[i];
    return nullptr;
}

void RememberClassified(const void* settings, WaterClass waterClass)
{
    const int slot = g_classified.count < kClassifiedSettingsCacheSize ? g_classified.count++ : g_classified.next;
    g_classified.next = (slot + 1) % kClassifiedSettingsCacheSize;
    g_classified.entries[slot] = {settings, waterClass};
}

bool MarkLiquidTypeLogged(uint32_t id)
{
    for (int i = 0; i < g_loggedLiquidTypeCount; ++i)
        if (g_loggedLiquidTypes[i] == id)
            return false;
    if (g_loggedLiquidTypeCount < kMaxLoggedLiquidTypes)
        g_loggedLiquidTypes[g_loggedLiquidTypeCount++] = id;
    return true;
}

void LogNewLiquid(const void* settings, const waterbridge::LiquidType& d, WaterClass waterClass)
{
    if (!d.inBank)
    {
        if (!g_loggedUnreadableSettings)
            VF_LOG_INFO("water: liquid settings %p are not in the client's settings bank; left to the client",
                        settings);
        g_loggedUnreadableSettings = true;
        return;
    }
    if (!MarkLiquidTypeLogged(d.id))
        return;
    if (!d.hasRow)
        VF_LOG_INFO("water: liquid type %u -> %s (no LiquidType row)", d.id, WaterClassLabel(waterClass));
    else
        VF_LOG_INFO("water: liquid type %u -> %s (sound bank %u, material %u, flags 0x%X, texture %s)", d.id,
                    WaterClassLabel(waterClass), d.soundBank, d.materialId, d.flags, d.texture0);
}

// ---- contacts ----

struct ContactCollector
{
    WaterContactFrame* frame;
    const float* centre;
};

void CollectContact(const waterbridge::WaterUnit& unit, void* user)
{
    auto* collector = static_cast<ContactCollector*>(user);
    WaterContact contact;
    contact.guid = unit.guid;
    std::memcpy(contact.position, unit.position, sizeof(contact.position));
    contact.surface = unit.surface;
    contact.radius = unit.radius;
    contact.height = unit.height;
    contact.swimming = unit.swimming;
    contact.onTransport = unit.onTransport;
    SelectWaterContact(*collector->frame, contact, collector->centre);
}
}

bool IsSupportedClient()
{
    return true;
}

void* GameD3DDevice()
{
    return waterbridge::D3DDevice();
}

bool CameraInLiquid()
{
    return waterbridge::CameraInLiquid();
}

void CaptureOpaqueState(IDirect3DDevice9* device)
{
    OpaqueState state = {};
    if (SUCCEEDED(device->GetViewport(&state.viewport)))
    {
        ApplyPendingGxViewportDepthRange(state.viewport);
        if (!waterbridge::ReadGxMatrices(state.cameraRelativeView, state.glProjection) ||
            !IsPerspective(state.glProjection))
            waterbridge::ReadSceneMatrices(state.cameraRelativeView, state.glProjection);
        state.valid = UsableOpaqueState(state);
    }
    g_opaque = state;
}

void CaptureOpaqueState(IDirect3DDevice9* device, const float* cameraRelativeView, const float* glProjection)
{
    OpaqueState state = {};
    std::memcpy(state.cameraRelativeView, cameraRelativeView, sizeof(state.cameraRelativeView));
    std::memcpy(state.glProjection, glProjection, sizeof(state.glProjection));
    state.valid = SUCCEEDED(device->GetViewport(&state.viewport)) && UsableOpaqueState(state);
    g_opaque = state;
}

bool HasOpaqueState()
{
    return g_opaque.valid;
}

bool OpaqueViewport(D3DVIEWPORT9& out)
{
    if (!g_opaque.valid)
        return false;
    out = g_opaque.viewport;
    return true;
}

void ClearOpaqueState()
{
    g_opaque.valid = false;
}

// The water pass reads only what the world pass captured and what the client's lighting holds; the fog's point
// light capture is not part of this build, so `withPointLights` and `upload` are accepted and ignored.
bool BuildFrameInputs(FrameInputs& out, bool, const PointLightUpload&)
{
    if (!g_opaque.valid)
        return false;

    std::memcpy(out.cameraRelativeView, g_opaque.cameraRelativeView, sizeof(out.cameraRelativeView));
    std::memcpy(out.glProjection, g_opaque.glProjection, sizeof(out.glProjection));
    out.viewport = g_opaque.viewport;
    waterbridge::ReadCamera(out.camPos, out.camTarget);

    waterbridge::Lighting light;
    if (!waterbridge::ReadLighting(light))
        return false;

    out.dayFraction = light.dayFraction;
    out.lightIsMoon = !(out.dayFraction >= light.sunDayStart && out.dayFraction <= light.sunDayEnd);
    const float* body = out.lightIsMoon ? light.moonPosition : light.sunPosition;
    for (int i = 0; i < 3; ++i)
        out.toLight[i] = body[i] - light.skyCenter[i];
    Normalize(out.toLight);

    out.fogColor = light.fogColor;
    out.fogStart = light.fogStart[kFrameInputsFogGroup];
    out.fogEnd = light.fogEnd[kFrameInputsFogGroup];
    out.sunColor = light.sunColor;
    out.directColor = light.directColor;
    out.ambientColor = light.ambientColor;
    out.inLiquid = CameraInLiquid();
    out.mapId = waterbridge::MapId();
    out.lightParams = SelectLightParams(light);
    out.zoneFogDistance = light.zoneFogDistance;
    out.clientGlowAmount = 0.0f;

    out.farClip = out.glProjection[14] / (1.0f - out.glProjection[10]);
    if (!(out.farClip > 10.0f && out.farClip < 100000.0f))
        out.farClip = waterbridge::ClientFarClip();
    if (!(out.farClip > 10.0f && out.farClip < 100000.0f))
        out.farClip = 1000.0f;

    return Finite(out.camPos, 3) && Finite(out.toLight, 3) && std::isfinite(out.fogEnd) &&
           std::isfinite(out.fogStart);
}

bool BuildWaterInputs(WaterInputs& out)
{
    out = {};
    waterbridge::Lighting light;
    if (!waterbridge::ReadLighting(light))
        return false;
    std::memcpy(out.skyColors, light.skyColors, sizeof(out.skyColors));
    std::memcpy(out.riverColors, light.riverColors, sizeof(out.riverColors));
    std::memcpy(out.oceanColors, light.oceanColors, sizeof(out.oceanColors));
    return true;
}

bool TransparentLiquidsQueued(const void* liquidRenderer)
{
    return waterbridge::TransparentLiquidsQueued(liquidRenderer);
}

WaterClass ClassifyWaterSettings(const void* liquidSettings)
{
    if (!liquidSettings)
        return WaterClass::None;

    const void* entries = nullptr;
    uint32_t count = 0;
    waterbridge::SettingsBankIdentity(entries, count);
    if (!entries || !count)
        return WaterClass::None;
    if (entries != g_classified.bankEntries || count != g_classified.bankCount)
    {
        g_classified = {};
        g_classified.bankEntries = entries;
        g_classified.bankCount = count;
    }
    if (const ClassifiedSettings* known = FindClassified(liquidSettings))
        return known->waterClass;

    waterbridge::LiquidType description = {};
    waterbridge::DescribeLiquid(liquidSettings, description);
    const WaterClass waterClass =
        description.inBank && description.hasRow
            ? ClassifyLiquid(description.id, description.soundBank, description.materialId, description.texture0)
            : WaterClass::None;
    RememberClassified(liquidSettings, waterClass);
    LogNewLiquid(liquidSettings, description, waterClass);
    return waterClass;
}

bool WaterContactsSupported()
{
    return true;
}

bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out)
{
    out = {};
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(centre[i]) || std::fabs(centre[i]) > 100000.0f)
            return false;
    WaterContactFrame captured;
    ContactCollector collector = {&captured, centre};
    waterbridge::ForEachUnitInLiquid(&CollectContact, &collector);
    out = captured;
    return true;
}

const char* LocalLightCaptureName(LocalLightCapture capture)
{
    switch (capture)
    {
    case LocalLightCapture::Captured:
        return "captured";
    case LocalLightCapture::UnsupportedClient:
        return "client code differs from build 12340";
    case LocalLightCapture::CameraOutOfRange:
        return "camera position out of range";
    case LocalLightCapture::DamagedTable:
        return "damaged light table";
    case LocalLightCapture::DisabledLight:
        return "disabled light in the table";
    case LocalLightCapture::NonPointLight:
        return "non-point light in the table";
    case LocalLightCapture::TableChanged:
        return "table changed during the walk";
    case LocalLightCapture::ReadFault:
        return "read fault";
    case LocalLightCapture::InteriorRejected:
        return "camera interior groups rejected";
    }
    return "unknown";
}
}
