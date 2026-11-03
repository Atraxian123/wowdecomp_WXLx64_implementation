// See water_driver.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see LICENSE)

#include "water_driver.h"

#include "bridge.h"
#include "client_ripple_sprites.h"
#include "config.h"
#include "d3d9_wrap.h"
#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "status_log.h"
#include "water_classify.h"
#include "water_data.h"
#include "water_renderer.h"

#include "wxl_module.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace wxl::water
{
namespace
{
constexpr unsigned kFlatWavesBit = 1u << 31;
constexpr size_t kDrawnWaterClassesTextSize = 96;
constexpr DWORD kConfigReloadIntervalMs = 1000;
constexpr DWORD kAdoptRetryIntervalMs = 2000;

bool g_installed = false;
bool g_failed = false;
bool g_faultLogged = false;

FogDevice* g_passDevice = nullptr;
FogDevice* g_resourcesDevice = nullptr;
bool g_passRanThisFrame = false;
bool g_shadedThisFrame = false;
bool g_ripplesAvailable = true;
unsigned g_classesThisPass = 0;
WaterFrameStatus g_status = {false, "waiting for the world to render"};
StatusLog g_log;
unsigned g_drawnClassesTextMask = 0;
char g_drawnClassesText[kDrawnWaterClassesTextSize] = "";
DWORD g_lastReload = 0;
DWORD g_lastAdoptTry = 0;

// ---- where the module lives: WXL\Modules\wxl_water\ beside wxl_water.dll ----

std::string ModuleDirectory()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ModuleDirectory), &self);
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir(path, n);
    const size_t slash = dir.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : dir.substr(0, slash + 1)) + "wxl_water\\";
}

// ---- exceptions: the render calls below run inside the client's frame, so a fault must stop the water, not it ----

int FaultFilter(unsigned code, const char* where)
{
    if (!g_faultLogged)
        VF_LOG_ERROR("exception 0x%08X in %s; water disabled for this session", code, where);
    g_faultLogged = true;
    return EXCEPTION_EXECUTE_HANDLER;
}

template <typename Fn>
bool Guarded(const char* where, Fn&& fn)
{
#if defined(_MSC_VER)
    __try
    {
        fn();
        return true;
    }
    __except (FaultFilter(GetExceptionCode(), where))
    {
        return false;
    }
#else
    try
    {
        fn();
        return true;
    }
    catch (...)
    {
        FaultFilter(0, where);
        return false;
    }
#endif
}

// ---- device ----

FogDevice* GameFogDevice()
{
    return WrapperOrLatestFogDevice(engine::GameD3DDevice());
}

// The wrapper binds a sampleable depth buffer to the client's device. It is made as soon as the device exists, and
// again after every Reset (the Reset releases it first, see OnDeviceLost). Retried at a low rate when it fails.
void EnsureAdopted()
{
    if (LatestFogDevice() || !engine::GameD3DDevice())
        return;
    const DWORD now = GetTickCount();
    if (g_lastAdoptTry && now - g_lastAdoptTry < kAdoptRetryIntervalMs)
        return;
    g_lastAdoptTry = now;
    AdoptExistingDevice();
}

void ReloadConfigAfterInterval()
{
    const DWORD now = GetTickCount();
    if (now - g_lastReload > kConfigReloadIntervalMs)
    {
        g_lastReload = now;
        GlobalConfig().ReloadIfChanged();
    }
}

// ---- status ----

void RecordSkip(const char* reason)
{
    g_status = {false, reason};
    const StatusLogLine line = g_log.Skip(reason);
    if (line.write)
        LogWrite(line.level, "water skipped: %s", reason);
}

void RecordIdle(const char* state)
{
    g_status = {false, state};
    const StatusLogLine line = g_log.Idle(state);
    if (line.write)
        LogWrite(line.level, "water idle: %s%s", state,
                 line.level == LogLevel::Info ? " (repeats are logged at LogLevel 2)" : "");
}

unsigned ClassBit(WaterClass waterClass)
{
    return 1u << static_cast<unsigned>(waterClass);
}

const char* DrawnClassesText(unsigned classMask, bool flatWaves)
{
    const unsigned key = classMask | (flatWaves ? kFlatWavesBit : 0u);
    if (key == g_drawnClassesTextMask)
        return g_drawnClassesText;
    g_drawnClassesTextMask = key;
    g_drawnClassesText[0] = 0;
    for (WaterClass waterClass : {WaterClass::Lake, WaterClass::River, WaterClass::Ocean, WaterClass::Interior})
    {
        if (!(classMask & ClassBit(waterClass)))
            continue;
        const size_t used = std::strlen(g_drawnClassesText);
        std::snprintf(g_drawnClassesText + used, sizeof(g_drawnClassesText) - used, "%s%s", used ? ", " : "",
                      WaterClassLabel(waterClass));
    }
    if (flatWaves)
    {
        const size_t used = std::strlen(g_drawnClassesText);
        std::snprintf(g_drawnClassesText + used, sizeof(g_drawnClassesText) - used, "; flat waves");
    }
    return g_drawnClassesText;
}

void RecordDrawn(unsigned classMask, bool flatWaves)
{
    g_status = {true, DrawnClassesText(classMask, flatWaves)};
    g_log.Drawn();
}

// ---- the water pass ----

enum class Arming
{
    Armed,
    Idle,
    Failed,
};

Arming ArmWaterPass(FogDevice* device, const Config& cfg, const char** reason)
{
    FrameInputs in = {};
    if (!engine::BuildFrameInputs(in, false, {}))
    {
        *reason = "invalid frame inputs";
        return Arming::Failed;
    }
    if (in.inLiquid)
    {
        *reason = "camera under water";
        return Arming::Idle;
    }
    WaterInputs water = {};
    if (!engine::BuildWaterInputs(water))
    {
        *reason = "the client's water colours are unavailable";
        return Arming::Failed;
    }
    water.stockFogApplies = true; // the client's own fog is never pushed out of range in this build
    water.contacts = {};
    if (cfg.waterRipples > 0.0f)
        engine::CaptureWaterContacts(in.camTarget, water.contacts);
    const char* skip = "";
    if (BeginWaterPass(device, in, water, cfg, &skip))
        return Arming::Armed;
    *reason = skip && *skip ? skip : "the water pass could not start";
    return Arming::Failed;
}

void AbortArmedWaterPass()
{
    FogDevice* device = g_passDevice;
    g_passDevice = nullptr;
    if (device)
        AbortWaterPass(device);
}

void FailWater()
{
    g_failed = true;
    GlobalClientRippleSprites().Restore();
    Guarded("water pass abort", [] { AbortArmedWaterPass(); });
    g_passDevice = nullptr;
}

void PassBeginUnsafe(const void* liquidRenderer)
{
    g_passRanThisFrame = true;
    g_passDevice = nullptr;
    g_classesThisPass = 0;
    const Config& cfg = GlobalConfig().Get();
    if (g_failed || !cfg.water)
        return;
    if (!engine::TransparentLiquidsQueued(liquidRenderer))
    {
        RecordIdle("no water in view");
        return;
    }
    FogDevice* device = GameFogDevice();
    if (!device)
    {
        RecordSkip("no fog device");
        return;
    }
    const char* reason = "the water pass could not start";
    g_passDevice = device;
    g_resourcesDevice = device;
    const Arming arming = ArmWaterPass(device, cfg, &reason);
    if (arming == Arming::Armed)
        return;
    g_passDevice = nullptr;
    if (arming == Arming::Idle)
        RecordIdle(reason);
    else
        RecordSkip(reason);
}

void PassEndUnsafe()
{
    if (!g_passDevice)
        return;
    const WaterPassEnd end = EndWaterPass(g_passDevice);
    g_passDevice = nullptr;
    g_shadedThisFrame = g_shadedThisFrame || end.shaded;
    if (end.shaded)
    {
        g_ripplesAvailable = end.ripplesAvailable;
        RecordDrawn(end.shadedClasses, end.flatWaves);
    }
    else if (!g_classesThisPass)
        RecordIdle("no water in view");
    else
        RecordSkip(end.skipReason && *end.skipReason ? end.skipReason : "the water pass shaded nothing");
}

void OnPassBegin(const void* liquidRenderer)
{
    if (!Guarded("water pass begin", [&] { PassBeginUnsafe(liquidRenderer); }))
        FailWater();
}

void OnPassEnd()
{
    if (!Guarded("water pass end", [] { PassEndUnsafe(); }))
        FailWater();
}

int OnTagDraw(const void* liquidSettings)
{
    if (!g_passDevice)
        return 0;
    bool tagged = false;
    const bool ok = Guarded("water draw classification", [&] {
        const WaterClass waterClass = engine::ClassifyWaterSettings(liquidSettings);
        TagWaterDraw(g_passDevice, waterClass);
        if (waterClass != WaterClass::None)
            g_classesThisPass |= ClassBit(waterClass);
        tagged = true;
    });
    if (!ok)
        FailWater();
    return tagged ? 1 : 0;
}

void OnUntagDraw()
{
    if (!g_passDevice)
        return;
    if (!Guarded("water draw untag", [] { UntagWaterDraw(g_passDevice); }))
        FailWater();
}

void OnOpaqueDone()
{
    const Config& cfg = GlobalConfig().Get();
    if (g_failed || !cfg.water)
        return;
    EnsureAdopted();
    FogDevice* device = GameFogDevice();
    if (!device)
        return;
    Guarded("opaque capture", [&] { engine::CaptureOpaqueState(RealDevice(device)); });
}

bool RingsReplaceClientSprites(const Config& cfg)
{
    return !g_failed && cfg.water && cfg.waterRipples > 0.0f && !cfg.waterClientSplashes && g_ripplesAvailable &&
           engine::WaterContactsSupported();
}

void ReleaseWaterWhenOff()
{
    if (!g_resourcesDevice || (!g_failed && GlobalConfig().Get().water))
        return;
    FogDevice* device = g_resourcesDevice;
    g_resourcesDevice = nullptr;
    if (device == GameFogDevice())
        ReleaseWaterResources(device);
}

void EndFrameUnsafe()
{
    ReloadConfigAfterInterval();
    if (g_passDevice)
    {
        AbortArmedWaterPass();
        RecordSkip("the water pass did not finish");
    }
    else if (!g_passRanThisFrame)
        RecordIdle("no liquid pass in this frame");
    g_passRanThisFrame = false;
    GlobalClientRippleSprites().Update(RingsReplaceClientSprites(GlobalConfig().Get()), g_shadedThisFrame,
                                       WaterClockSeconds());
    g_shadedThisFrame = false;
    ReleaseWaterWhenOff();
}

void OnFrameEnd()
{
    if (!Guarded("water frame end", [] { EndFrameUnsafe(); }))
        FailWater();
}

// ---- device lifetime: the wrapper owns a DEFAULT-pool depth buffer and the water's render targets, none of which
// may outlive the client's own Reset ----

void OnDeviceLost(void*, const void*)
{
    AbortArmedWaterPass();
    g_resourcesDevice = nullptr;
    ReleaseAdoptedDevice();
}

void OnDeviceReset(void*, const void*)
{
    g_lastAdoptTry = 0;
    EnsureAdopted();
}
}

bool Start(const WxlApi* api)
{
    if (!api || !WXL_API_HAS(api, getService) || !WXL_API_HAS(api, iniGetInt) || !api->subscribe)
        return false;
    if (api->iniGetInt("Water", "Enabled", 0) == 0)
    {
        wxl::LogCat("WATER", "modern water off ([Water] Enabled is 0 or absent)");
        return false;
    }

    size_t size = 0;
    const auto* host = static_cast<const WxlWaterService*>(api->getService(WXL_SERVICE_WATER, &size));
    if (!host || size < sizeof(WxlWaterService) || host->structSize < sizeof(WxlWaterService))
    {
        wxl::LogCat("WATER", "modern water off: the client offers no (or an older) water service");
        return false;
    }
    waterbridge::Bind(host);

    const std::string dir = ModuleDirectory();
    LogOpen((dir + "wxl-modern-water.log").c_str());
    GlobalConfig().Load(dir + "wxl-modern-water.ini");
    if (!GlobalWaterData().Load(dir + "waterdata.bin"))
    {
        wxl::LogCat("WATER", "modern water off: %swaterdata.bin is missing or invalid", dir.c_str());
        waterbridge::Bind(nullptr);
        return false;
    }

    if (api->subscribe("OnDeviceLost", &OnDeviceLost, nullptr) != 0 ||
        api->subscribe("OnDeviceReset", &OnDeviceReset, nullptr) != 0)
    {
        wxl::LogCat("WATER", "modern water off: the client has no device events");
        waterbridge::Bind(nullptr);
        return false;
    }

    GlobalClientRippleSprites().Bind(waterbridge::ClientRippleGate(), ClientCodeView());

    WxlWaterCallbacks callbacks = {};
    callbacks.structSize = sizeof(callbacks);
    callbacks.opaqueDone = &OnOpaqueDone;
    callbacks.passBegin = &OnPassBegin;
    callbacks.passEnd = &OnPassEnd;
    callbacks.tagDraw = &OnTagDraw;
    callbacks.untagDraw = &OnUntagDraw;
    callbacks.frameEnd = &OnFrameEnd;
    host->setCallbacks(&callbacks);

    g_installed = true;
    EnsureAdopted();
    wxl::LogCat("WATER", "modern water active (settings and log in %s)", dir.c_str());
    return true;
}

void Stop()
{
    if (!g_installed)
        return;
    g_installed = false;
    if (const WxlWaterService* host = waterbridge::Host())
        host->setCallbacks(nullptr); // the client stops calling in before anything below goes away
    // Called as the process exits: the client's device and the module's resources on it go with the process, and
    // touching COM objects the client may already have released is the one thing that could fault here.
    Guarded("water shutdown", [] {
        g_passDevice = nullptr;
        GlobalClientRippleSprites().Restore();
    });
    waterbridge::Bind(nullptr);
}
}

WaterFrameStatus LastWaterFrameStatus()
{
    return wxl::water::g_status;
}
