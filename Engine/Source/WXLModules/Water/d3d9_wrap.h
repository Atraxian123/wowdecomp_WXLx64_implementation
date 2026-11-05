#pragma once

#include "config.h"
#include "engine.h"
#include "grading_renderer.h"
#include "msaa_depth.h"
#include "transparent_fog.h"
#include "water_types.h"

#include <d3d9.h>

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

void SetRealDirect3DCreate9(Direct3DCreate9Fn fn);
IDirect3D9* WINAPI WrappedDirect3DCreate9(UINT sdkVersion);

// WXL loads an extension after the client has already resolved its delay-loaded Direct3DCreate9, so
// the GetProcAddress filter can no longer be installed. This wraps the device the other way: it
// creates a throwaway factory from the resolved entry point, patches the shared IDirect3D9 vtable so
// the engine's own factory hands CreateDevice here, and returns whether that succeeded.
bool InstallLateFactoryWrapping(Direct3DCreate9Fn create9);

// WXL loads extensions after the client already created its D3D9 device, so factory wrapping is
// impossible. This adopts the live device instead: wrap it, bind an INTZ depth and point the engine's
// graphics-device singleton at the wrapper.
bool AdoptExistingDevice();

// Undoes AdoptExistingDevice before the client resets its device.
void ReleaseAdoptedDevice();

// The depth surface the client should bind instead of `engineDepth` (an IDirect3DSurface9*): the sampleable INTZ depth
// of the adopted device when `engineDepth` is the client's own scene depth, else `engineDepth` itself. Called from the
// client's own depth binds, so it works whether or not the device's vtable hook is reached.
void* MapEngineDepth(void* engineDepth);

void AllowFogOnNewDevices(bool allowedOnNewDevices);

class FogDevice;
FogDevice* LatestFogDevice();
MultisamplingStatus CurrentMultisamplingStatus();
bool ReadSceneDepth(FogDevice* device, const DepthTexel* texels, int count, float* values);
FogDevice* WrapperOrLatestFogDevice(void* gameDevice);
bool IsWrapperOf(FogDevice* device, void* gameDevice);
IDirect3DDevice9* RealDevice(FogDevice* device);
void ForceDepthWrite(FogDevice* device, bool force);
void SuppressDepthWrite(FogDevice* device, bool suppress);
bool RenderFog(FogDevice* device, const FrameInputs& in, const Config& cfg, const char** skipReason);
bool RenderFog(FogDevice* device, const FrameInputs& in, const Config& cfg, FogPass pass, const char** skipReason);
bool FogReadyToRender(FogDevice* device, const D3DVIEWPORT9& vp, const char** skipReason);
bool RenderGodRaysAfterWorld(FogDevice* device, const char** skipReason);
StockFogFit LastStockFogFit(FogDevice* device);
bool AdaptiveLightingHistory(FogDevice* device);
void DrawnFogShaders(FogDevice* device, IDirect3DPixelShader9** march, IDirect3DPixelShader9** composite,
                     IDirect3DPixelShader9** splitComposite);
float DrawnFogGlowCompensation(FogDevice* device);
bool BeginWaterPass(FogDevice* device, const FrameInputs& in, const WaterInputs& water, const Config& cfg,
                    const char** skipReason);
void TagWaterDraw(FogDevice* device, WaterClass waterClass);
void UntagWaterDraw(FogDevice* device);

struct WaterPassEnd
{
    bool shaded = false;
    const char* skipReason = "no fog device";
    bool flatWaves = false;
    unsigned shadedClasses = 0;
    bool ripplesAvailable = false;
};

WaterPassEnd EndWaterPass(FogDevice* device);
void AbortWaterPass(FogDevice* device);
void ReleaseWaterResources(FogDevice* device);
unsigned HeldWaterResources(FogDevice* device);
bool WaterPassArmed(FogDevice* device);
int WaterFoamMaskPool(FogDevice* device);
int UploadedWaterMasks(FogDevice* device);
int RequiredWaterMasks(FogDevice* device);
int LastWaterShadingVariant(FogDevice* device);

struct WaterRippleStats;
struct WaterRippleShading;
void ReadWaterRippleStats(FogDevice* device, WaterRippleStats& out);
void ReadWaterRippleShading(FogDevice* device, WaterRippleShading& out);
bool GradeWorld(FogDevice* device, const D3DVIEWPORT9& world, const float* curve, float strength,
                const char** skipReason);
void ReleaseGrading(FogDevice* device);
GradingStats GradingStatsOf(FogDevice* device);
