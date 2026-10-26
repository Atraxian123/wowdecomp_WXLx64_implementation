// The table every module receives. See ApiHost.h and Engine/Source/WXL/Sdk/wxl_api.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Every entry is a thin C-ABI face over something the core already has. Where a function takes or returns a
// C++ type (std::vector in the storage layer, bool in the ini helpers) this file is the only place that
// converts, so the types never reach a module.

#include "Engine/Source/WXLApi/ApiHost.h"

#include "Engine/Source/WXL/Event.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/ModuleRegistry.h"
#include "Engine/Source/WXL/Services.h"
#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXLApi/ModelAccess.h"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace wxl::api
{
    namespace
    {
        namespace eq = wxl::scripts::equipextension;

        void Log(const char* category, const char* message)
        {
            wxl::LogCat(category && *category ? category : "MODULE", "%s", message ? message : "");
        }

        int LogThrottle(const char* key, uint32_t first, uint32_t every)
        {
            return wxl::LogThrottle(key ? key : "", first, every) ? 1 : 0;
        }

        // Events are matched by name so a module survives the core adding or reordering events.
        int Subscribe(const char* eventName, WxlEventHandler handler, void* user)
        {
            if (!eventName || !handler) return 1;
            const uint32_t count = static_cast<uint32_t>(wxl::events::Event::Count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto e = static_cast<wxl::events::Event>(i);
                if (std::strcmp(wxl::events::EventName(e), eventName) == 0)
                {
                    wxl::events::Subscribe(e, handler, user);   // same signature: void (*)(void*, const void*)
                    return 0;
                }
            }
            wxl::LogCat("MODULE", "subscribe: unknown event '%s'", eventName);
            return 2;
        }

        int IniGetString(const char* section, const char* key, char* out, size_t cap)
        {
            return eq::WxlIniGetString(section, key, out, cap) ? 1 : 0;
        }
        int IniGetInt(const char* section, const char* key, int defaultValue)
        {
            return eq::WxlIniGetInt(section, key, defaultValue);
        }
        int IniGetBool(const char* section, const char* key, int defaultValue)
        {
            return eq::WxlIniGetBool(section, key, defaultValue != 0) ? 1 : 0;
        }
        uint64_t IniStamp() { return eq::WxlIniStamp(); }

        int ReadFile(const char* name, WxlBuffer* out)
        {
            if (!name || !out) return 0;
            out->data = nullptr;
            out->size = 0;
            std::vector<uint8_t> bytes;
            if (!wxl::storage::ReadNativeQuiet(name, bytes) || bytes.empty()) return 0;
            void* copy = std::malloc(bytes.size());
            if (!copy) return 0;
            std::memcpy(copy, bytes.data(), bytes.size());
            out->data = copy;
            out->size = bytes.size();
            return 1;
        }

        void FreeBuffer(WxlBuffer* buffer)
        {
            if (!buffer) return;
            std::free(buffer->data);
            buffer->data = nullptr;
            buffer->size = 0;
        }

        // Virtual paths are minted by the client's own VirtualPath and, as modules take that over, by modules.
        int IsVirtualKey(const char* name)
        {
            return name && (eq::VPathIsVirtualKey(name) || wxl::registry::IsVirtualKey(name)) ? 1 : 0;
        }

        const char* WorkingDirectory() { return wxl::WorkingDirectory(); }

        // ---- virtual paths: the core's table, behind plain C signatures ----
        int VPathPopulateGlobal(const WxlVPathGlobal* p)
        {
            if (!p || p->structSize < sizeof(WxlVPathGlobal)) return 0;
            return eq::VPathPopulateGlobal(p->realMdxPath, p->itemDisplayId, p->texPath, p->materialPatchSpec, p->geoIds,
                                           p->geoCount, p->evictable != 0, p->outVirtualPath, p->outVirtualPathSz,
                                           p->evictionPool ? p->evictionPool : "Creature", p->forceBake != 0,
                                           p->mirrorAcrossY != 0, p->keySuffix) ? 1 : 0;
        }
        int VPathPopulateGlobalSharedKey(const char* keySourcePath, const char* byteSourcePath, uint32_t itemDisplayId,
                                         const char* texPath, const char* materialPatchSpec, const uint16_t* geoIds,
                                         uint32_t geoCount, char* outVirtualPath, size_t outVirtualPathSz)
        {
            return eq::VPathPopulateGlobalSharedKey(keySourcePath, byteSourcePath, itemDisplayId, texPath, materialPatchSpec,
                                                    geoIds, geoCount, outVirtualPath, outVirtualPathSz) ? 1 : 0;
        }
        int VPathPopulate(void* cmo, const char* realMdxPath, const uint16_t* geoIds, uint32_t geoCount, const char* texPath,
                          uint32_t variantKey, const char* materialPatchSpec)
        {
            return eq::VPathPopulate(cmo, realMdxPath, geoIds, geoCount, texPath, variantKey, materialPatchSpec) ? 1 : 0;
        }
        void VPathEvictCmo(void* cmo) { eq::VPathEvictCmo(cmo); }
        int VPathDecodeGlobalVirtualKey(const char* name, char* outNorm, size_t outNormSz, uint32_t* outDisplayId)
        {
            return eq::VPathDecodeGlobalVirtualKey(name, outNorm, outNormSz, outDisplayId) ? 1 : 0;
        }

        // The core's lazy resolvers are bare function pointers with no user data, so a module's resolver
        // (a C function returning int) is reached through one of a fixed set of trampolines.
        constexpr size_t kMaxResolvers = 8;
        WxlVPathLazyResolver g_resolvers[kMaxResolvers] = {};
        template <size_t N> bool Trampoline(const char* name) { return g_resolvers[N] && g_resolvers[N](name) != 0; }
        template <size_t... I> constexpr std::array<eq::VPathLazyResolver, sizeof...(I)> MakeTrampolines(std::index_sequence<I...>)
        {
            return { &Trampoline<I>... };
        }
        void VPathRegisterLazyResolver(WxlVPathLazyResolver resolver)
        {
            static std::mutex lock;
            static size_t used = 0;
            static const auto tramps = MakeTrampolines(std::make_index_sequence<kMaxResolvers>());
            if (!resolver) return;
            std::lock_guard<std::mutex> g(lock);
            if (used >= kMaxResolvers) { wxl::LogCat("MODULE", "vpathRegisterLazyResolver: more than %zu resolvers, ignored", kMaxResolvers); return; }
            g_resolvers[used] = resolver;
            eq::VPathRegisterLazyResolver(tramps[used]);
            ++used;
        }

        int RegisterService(const char* name, const void* table, size_t size) { return wxl::services::Register(name, table, size) ? 0 : 1; }
        const void* GetService(const char* name, size_t* size) { return wxl::services::Get(name, size); }

        int RegisterFileProvider(WxlFileProviderFn fn, void* user)   { return wxl::registry::RegisterFileProvider(fn, user); }
        int RegisterVirtualKeyQuery(WxlVirtualKeyFn fn, void* user)  { return wxl::registry::RegisterVirtualKeyQuery(fn, user); }

        int FileExists(const char* name) { return wxl::storage::ExistsNative(name) ? 1 : 0; }

        const WxlApi g_table = {
            WXL_ABI_VERSION,
            sizeof(WxlApi),
            &Log,
            &LogThrottle,
            &Subscribe,
            &IniGetString,
            &IniGetInt,
            &IniGetBool,
            &IniStamp,
            &ReadFile,
            &FreeBuffer,
            &IsVirtualKey,
            &wxl::api::model::ReadInstance,
            &wxl::api::model::ReadBones,
            &wxl::api::model::WritePalette,
            &WorkingDirectory,
            &VPathPopulateGlobal,
            &VPathPopulateGlobalSharedKey,
            &VPathPopulate,
            &VPathEvictCmo,
            &VPathRegisterLazyResolver,
            &VPathDecodeGlobalVirtualKey,
            &RegisterService,
            &GetService,
            &RegisterFileProvider,
            &RegisterVirtualKeyQuery,
            &FileExists,
        };
    }

    const WxlApi* Table() { return &g_table; }
}
