/*
 * The "water host" service: what the modern water module reads from the client, and the call sites it asks the
 * client to make. Plain C, offered by the core under WXL_SERVICE_WATER and looked up through WxlApi::getService.
 * Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
 *
 * The water module is a renderer: it draws through the client's IDirect3DDevice9 and needs a handful of the
 * client's own values (camera, light, liquid types, units standing in water). It reads them through this table so
 * it includes no engine header. The device itself crosses as an opaque pointer to a COM object; the module links
 * the system's d3d9 for it.
 *
 * Everything here runs on the render thread.
 */
#ifndef WXL_WATER_API_H
#define WXL_WATER_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_SERVICE_WATER "wxl.water.v1"

/* Day / night lighting and the water colours the client has interpolated for the camera's position. Colours are
 * the client's packed BGRA words. */
typedef struct WxlWaterLighting
{
    float    dayFraction;
    float    sunPosition[3];
    float    moonPosition[3];
    float    skyCenter[3];
    float    sunDayStart;
    float    sunDayEnd;
    uint32_t fogColor;
    float    fogStart[2];
    float    fogEnd[2];
    uint32_t sunColor;
    uint32_t directColor;
    uint32_t ambientColor;
    float    zoneFogDistance;
    float    stormBlend;
    int32_t  screenEffectLightSlot;
    uint32_t skyColors[6];
    uint32_t riverColors[2];
    uint32_t oceanColors[2];
} WxlWaterLighting;

/* What the client's liquid type tables say about one liquid material settings object. */
typedef struct WxlWaterLiquidType
{
    int32_t  inBank;       /* the settings object is one of the client's per-liquid-type settings */
    int32_t  hasRow;       /* and its LiquidType row exists */
    uint32_t id;
    uint32_t flags;
    uint32_t soundBank;
    uint32_t materialId;
    char     texture0[128];
} WxlWaterLiquidType;

/* A visible unit that stands in liquid deep enough to cross its body. */
typedef struct WxlWaterUnit
{
    uint64_t guid;
    float    position[3];
    float    surface;
    float    radius;
    float    height;
    int32_t  swimming;
    int32_t  onTransport;
} WxlWaterUnit;

typedef void (*WxlWaterUnitVisitor)(const WxlWaterUnit* unit, void* user);

/* The five moments of the world frame the module acts at. Any member may be NULL. */
typedef struct WxlWaterCallbacks
{
    uint32_t structSize;                                  /* sizeof(WxlWaterCallbacks), set by the caller */
    void (*opaqueDone)(void);                             /* the opaque M2 pass has drawn */
    void (*passBegin)(const void* liquidRenderer);        /* before the transparent liquid pass */
    void (*passEnd)(void);                                /* after it */
    int  (*tagDraw)(const void* liquidSettings);          /* a water material is about to draw; 1 = tagged */
    void (*untagDraw)(void);                              /* and it has drawn */
    void (*frameEnd)(void);                               /* the world render is over */
    void* (*mapDepth)(void* depthSurface);                /* the client binds this IDirect3DSurface9 as its depth: return the one to bind instead (the same pointer to leave it) */
} WxlWaterCallbacks;

typedef struct WxlWaterService
{
    uint32_t structSize;   /* sizeof(WxlWaterService) in the core: members past it do not exist */

    /* the client's live IDirect3DDevice9, or NULL before the Gx device exists / when it is not a D3D9 device */
    void*   (*d3dDevice)(void);
    int     (*cameraInLiquid)(void);

    /* 16 floats each: the Gx view stack top (camera relative) and the Gx projection; the world scene's own
     * matrices for when the Gx ones are not usable */
    int     (*readGxMatrices)(float view[16], float projection[16]);
    void    (*readSceneMatrices)(float view[16], float projection[16]);
    int     (*readViewportDepthRange)(float* minZ, float* maxZ);

    void    (*readCamera)(float position[3], float target[3]);
    float   (*clientFarClip)(void);
    int32_t (*mapId)(void);
    int     (*readLighting)(WxlWaterLighting* out);

    int     (*describeLiquid)(const void* liquidSettings, WxlWaterLiquidType* out);
    /* identity of the settings table, so a reload invalidates anything cached against it */
    void    (*settingsBankIdentity)(const void** entries, uint32_t* count);
    int     (*transparentLiquidsQueued)(const void* liquidRenderer);

    void    (*forEachUnitInLiquid)(WxlWaterUnitVisitor visitor, void* user);

    /* the client's own splash and wake sprites switch (CWorld::s_waterRipples) */
    volatile int32_t* (*clientRippleGate)(void);

    /* the module's moments; copied, so the struct need not outlive the call. NULL clears them. */
    void    (*setCallbacks)(const WxlWaterCallbacks* callbacks);
} WxlWaterService;

#ifdef __cplusplus
}
#endif

#endif /* WXL_WATER_API_H */
