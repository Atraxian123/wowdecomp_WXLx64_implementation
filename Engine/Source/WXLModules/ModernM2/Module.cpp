// The modern M2 module's entry points. See Engine/Source/WXL/Sdk/wxl_api.h and wxl_modernm2_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Registers the service "wxl.modernm2.v1": the client's M2 loader (Engine/Source/Model2/ModernM2/ModernM2.cpp)
// calls it at the load steps of CM2Shared. Needs the listfile module (wxl_listfile.dll) only to name a texture by
// its TXID file id, and looks that service up when a texture is named, so load order between the two does not matter.

#include "Internal.hpp"

#include "wxl_module.hpp"

#include <cstring>

WXL_DEFINE_MODULE_ABI()

namespace
{
    namespace m2 = wxl::m2mod;

    // No exception leaves the module: the client calls these from its async load threads.
    int SvcPrepareLoad(const char* name, const void* file, uint32_t fileSize, const WxlM2Alloc* mem, WxlM2PrepareResult* out)
    {
        if (!out || out->structSize < sizeof(WxlM2PrepareResult)) return WXL_M2_NOT_MODERN;
        out->status = WXL_M2_NOT_MODERN;
        out->body = nullptr;
        out->bodySize = out->reserveBase = out->reserveCap = out->version = out->txidCount = 0;
        if (!mem || mem->structSize < sizeof(WxlM2Alloc) || !mem->alloc || !mem->release) return WXL_M2_NOT_MODERN;
        try { out->status = static_cast<uint32_t>(m2::PrepareLoad(name, file, fileSize, mem, out)); }
        catch (...) { out->status = WXL_M2_FAILED; }
        return static_cast<int>(out->status);
    }

    void SvcFinishWalk(void* model)
    {
        try { m2::FinishWalk(model); } catch (...) {}
    }

    int SvcTextureName(uint32_t fileId, char* out, size_t cap)
    {
        if (!out || !cap) return WXL_M2_TEXNAME_NONE;
        try { return m2::TextureName(fileId, out, cap); } catch (...) { return WXL_M2_TEXNAME_NONE; }
    }

    int SvcFinalizeSkin(const WxlM2Alloc* mem, WxlM2SkinJob* job)
    {
        if (!job || job->structSize < sizeof(WxlM2SkinJob) || !mem || mem->structSize < sizeof(WxlM2Alloc)) return 1;
        try { return m2::FinalizeSkin(mem, job); } catch (...) { return 1; }
    }

    int SvcUnwrapAnim(void* buffer, uint32_t* size)
    {
        try { return m2::UnwrapAnim(buffer, size); } catch (...) { return 0; }
    }

    // Static, valid for the life of the process (the client keeps the pointer).
    const WxlModernM2Services g_services = {
        sizeof(WxlModernM2Services),
        WXL_M2_VERSION_MIN,
        WXL_M2_VERSION_MAX,
        &SvcPrepareLoad,
        &SvcFinishWalk,
        &SvcTextureName,
        &SvcFinalizeSkin,
        &SvcUnwrapAnim,
    };
}

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (info)
    {
        std::strncpy(info->name, "modernm2", sizeof(info->name) - 1);
        info->versionMajor = 1;
        info->versionMinor = 0;
    }

    if (!api || api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, getService) || !api->registerService || !api->log)
        return 1;
    wxl::module::Bind(api);

    if (api->registerService(WXL_SERVICE_MODERNM2, &g_services, sizeof(g_services)) != 0)
    {
        wxl::LogCat("M2", "the service " WXL_SERVICE_MODERNM2 " is already registered");
        return 3;
    }

    wxl::LogCat("M2", "modern M2 module ready: inner versions %u-%u", WXL_M2_VERSION_MIN, WXL_M2_VERSION_MAX);
    return 0;
}

WXL_MODULE_API void WxlModuleShutdown(void)
{
    // Nothing to tear down: the module keeps no state between calls.
}
