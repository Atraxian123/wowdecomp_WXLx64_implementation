// See WaterHost.h. Everything the modern water module (Engine/Source/WXLModules/Water) needs from the decompiled
// client: the module itself includes no engine header, so each value is read here, by name, behind the C table
// in Engine/Source/WXL/Sdk/wxl_water_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)

#include "WaterHost.h"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.h"
#include "Engine/Source/Gx/CGxDeviceD3d9Ex/CGxDeviceD3d9Ex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/WXL/Sdk/wxl_water_api.h"
#include "Engine/Source/WXL/Services.h"
#include "Engine/Source/WXL/WaterHooks.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/WGUID.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapEntity.h"
#include "WoW/Source/WorldClient/MapLoad.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldScene.h"

#include <cstring>

static_assert(sizeof(C44Matrix) == 16 * sizeof(float), "the matrix reads below copy 16 floats");

namespace wxl::api
{
namespace
{
void CopyMatrix(const C44Matrix& m, float out[16]) { std::memcpy(out, &m, sizeof(float) * 16); }
void CopyVec(const C3Vector& v, float out[3]) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }
uint32_t Colour(const CImVector& c) { uint32_t v; std::memcpy(&v, &c, sizeof(v)); return v; }

// The CMapEntity flag bits the client sets for "a liquid was found under this unit" and "its surface crosses the
// unit's body".
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
// The day window the client uses to choose sun or moon as the light: DNClouds::SetupBumpMap.
constexpr float kSunDayStart = 0.2013889f;
constexpr float kSunDayEnd = 0.9236111f;

void* D3DDevice()
{
    if (!g_theGxDevicePtr)
        return nullptr;
    switch (g_theGxDevicePtr->DeviceApi())
    {
    case GxApi_D3d9:
        return static_cast<CGxDeviceD3d*>(g_theGxDevicePtr)->m_d3dDevice;
    case GxApi_D3d9Ex:
        return static_cast<CGxDeviceD3d9Ex*>(g_theGxDevicePtr)->m_d3dDevice;
    default:
        return nullptr;
    }
}

int CameraInLiquid()
{
    return World::SceneCamLiquidStatus(nullptr) != 0;
}

int ReadGxMatrices(float view[16], float projection[16])
{
    if (!g_theGxDevicePtr)
        return 0;
    C44Matrix v;
    GxXformView(v);
    CopyMatrix(v, view);
    CopyMatrix(g_theGxDevicePtr->m_projection, projection);
    return 1;
}

void ReadSceneMatrices(float view[16], float projection[16])
{
    CopyMatrix(CWorldScene::viewMatrix, view);
    CopyMatrix(CWorldScene::projMatrix, projection);
}

int ReadViewportDepthRange(float* minZ, float* maxZ)
{
    if (!g_theGxDevicePtr)
        return 0;
    float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f, y1 = 0.0f;
    GxXformViewport(x0, x1, y0, y1, *minZ, *maxZ);
    return 1;
}

void ReadCamera(float position[3], float target[3])
{
    CopyVec(CWorldScene::s_activeWorldView, position);
    CopyVec(CWorldScene::camTarget, target);
}

float ClientFarClip()
{
    return World::s_farClip;
}

int32_t MapId()
{
    return static_cast<int32_t>(ClntObjMgrGetMapID());
}

int ReadLighting(WxlWaterLighting* out)
{
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    *out = {};
    out->dayFraction = info.m_dayProgression;
    CopyVec(DayNight::g_planets.m_sun.m_position, out->sunPosition);
    CopyVec(DayNight::g_planets.m_moon.m_position, out->moonPosition);
    CopyVec(info.m_cameraPos, out->skyCenter); // the sky is centred on the camera
    out->sunDayStart = kSunDayStart;
    out->sunDayEnd = kSunDayEnd;

    // The client keeps two fog groups; the module reads the second for the frame's fog colour and range.
    out->fogColor = Colour(info.m_fogInterior.m_color);
    out->fogStart[0] = info.m_fog.m_start;
    out->fogEnd[0] = info.m_fog.m_end;
    out->fogStart[1] = info.m_fogInterior.m_start;
    out->fogEnd[1] = info.m_fogInterior.m_end;

    const DayNight::CurrentLight& bands = info.m_bands;
    out->ambientColor = Colour(bands.m_ambient);
    out->directColor = Colour(bands.m_diffuse);
    out->sunColor = Colour(bands.m_sunCloudColors[0]);
    out->zoneFogDistance = bands.m_fogEnd;
    out->stormBlend = info.m_stormBlend;
    out->screenEffectLightSlot = static_cast<int32_t>(info.m_overrideLightParamsId);
    for (int i = 0; i < 6; ++i)
        out->skyColors[i] = Colour(bands.m_skyColors[i]);
    out->oceanColors[0] = Colour(bands.m_waterColors[0]);
    out->oceanColors[1] = Colour(bands.m_waterColors[1]);
    out->riverColors[0] = Colour(bands.m_waterColors[2]);
    out->riverColors[1] = Colour(bands.m_waterColors[3]);
    return 1;
}

void SettingsBankIdentity(const void** entries, uint32_t* count)
{
    *entries = Liquid::CSettingsBank::s_settings.m_data;
    *count = Liquid::CSettingsBank::s_settings.Count();
}

int DescribeLiquid(const void* settings, WxlWaterLiquidType* out)
{
    *out = {};
    auto& bank = Liquid::CSettingsBank::s_settings;
    for (uint32_t i = 0; i < bank.Count(); ++i)
    {
        if (static_cast<const void*>(bank[i]) != settings)
            continue;
        out->inBank = 1;
        out->id = i;
        const LiquidTypeRec* row = g_liquidTypeDB.GetRecord(static_cast<int32_t>(i));
        out->hasRow = row != nullptr;
        if (row)
        {
            out->flags = static_cast<uint32_t>(row->m_flags);
            out->soundBank = static_cast<uint32_t>(row->m_soundBank);
            out->materialId = static_cast<uint32_t>(row->m_materialID);
        }
        const auto* material = static_cast<const Liquid::CMaterialSettings*>(settings);
        std::memcpy(out->texture0, material->m_textureNames[0], sizeof(out->texture0));
        out->texture0[sizeof(out->texture0) - 1] = 0;
        return 1;
    }
    return 1; // read fine, just not one of the bank's settings
}

int TransparentLiquidsQueued(const void* liquidRenderer)
{
    if (!liquidRenderer)
        return 0;
    return const_cast<Liquid::CRenderer*>(static_cast<const Liquid::CRenderer*>(liquidRenderer))->m_instances[1].Count() != 0;
}

struct UnitWalk
{
    WxlWaterUnitVisitor visitor;
    void* user;
};

int32_t UnitCallback(WOWGUID guid, void* param)
{
    auto* walk = static_cast<UnitWalk*>(param);

    CGObject_C* object = ClntObjMgrObjectPtr(guid, static_cast<TypeMask>(TYPEMASK_OBJECT), __FILE__, __LINE__);
    if (!object || !object->m_obj || !(static_cast<uint32_t>(object->m_obj->m_type) & TYPEMASK_UNIT))
        return 1;

    CGUnit_C* unit = static_cast<CGUnit_C*>(object);
    const CMapEntity* entity = unit->m_worldObject;
    if (!entity)
        return 1;
    const uint32_t liquid = entity->m_staticFlags;
    if (!(liquid & kEntityLiquidFound) || !(liquid & kEntitySurfaceCrossesBody))
        return 1;

    const CMovement_C& movement = unit->m_movementData;
    WxlWaterUnit u = {};
    u.guid = guid.m_guid;
    u.onTransport = movement.m_transportGuid.m_guid != 0;
    // On a transport the unit's own position is relative to it; the entity holds the world position.
    CopyVec(u.onTransport ? entity->m_position : movement.m_position, u.position);
    u.surface = entity->m_liquidHeight;
    u.radius = movement.m_collisionRadius;
    u.height = movement.m_collisionHeight;
    u.swimming = (movement.m_flags & kSwimmingMovementFlag) != 0;
    walk->visitor(&u, walk->user);
    return 1;
}

void ForEachUnitInLiquid(WxlWaterUnitVisitor visitor, void* user)
{
    UnitWalk walk = { visitor, user };
    ClntObjMgrEnumVisibleObjects(UnitCallback, &walk);
}

volatile int32_t* ClientRippleGate()
{
    return reinterpret_cast<volatile int32_t*>(&CWorld::s_waterRipples);
}

// The module's callbacks become the client's call-site table (Engine/Source/WXL/WaterHooks.h); the members have
// the same signatures, so it is a copy.
void SetCallbacks(const WxlWaterCallbacks* callbacks)
{
    waterhooks::Callbacks& table = waterhooks::Table();
    table = {};
    if (!callbacks)
        return;
    table.opaqueDone = callbacks->opaqueDone;
    table.passBegin = callbacks->passBegin;
    table.passEnd = callbacks->passEnd;
    table.tagDraw = callbacks->tagDraw;
    table.untagDraw = callbacks->untagDraw;
    table.frameEnd = callbacks->frameEnd;
}

const WxlWaterService kService = {
    sizeof(WxlWaterService),
    &D3DDevice,
    &CameraInLiquid,
    &ReadGxMatrices,
    &ReadSceneMatrices,
    &ReadViewportDepthRange,
    &ReadCamera,
    &ClientFarClip,
    &MapId,
    &ReadLighting,
    &DescribeLiquid,
    &SettingsBankIdentity,
    &TransparentLiquidsQueued,
    &ForEachUnitInLiquid,
    &ClientRippleGate,
    &SetCallbacks,
};
}

void RegisterWaterHost()
{
    wxl::services::Register(WXL_SERVICE_WATER, &kService, sizeof(kService));
}
}
