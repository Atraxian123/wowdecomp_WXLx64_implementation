// C++ face over the module API, for code that was written inside the client and keeps its names.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The built-in extensions (weapon, creature, equip) were written against the core's C++ headers: wxl::LogCat,
// wxl::storage::ReadNative, the EventScript base class, the equipextension:: virtual-path functions. This header
// offers the same names, implemented on the C ABI of wxl_api.h, so that code moves into a module by changing its
// #include lines and nothing else. A new module is free to call the table directly instead.
//
// Header-only. A module calls wxl::module::Bind(api) first thing in WxlModuleInit; every function below is a
// harmless no-op / false until then.
#pragma once

#include "wxl_api.h"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------------------------------------
namespace wxl::module
{
    /// This module's copy of the table pointer (one per DLL: inline functions share their statics inside one image).
    inline const WxlApi*& ApiRef() { static const WxlApi* api = nullptr; return api; }
    inline const WxlApi* Api() { return ApiRef(); }
    inline void Bind(const WxlApi* api) { ApiRef() = api; }
}

// ---------------------------------------------------------------------------------------------------------
namespace wxl
{
    inline void LogCatV(const char* category, const char* format, va_list args)
    {
        const WxlApi* api = module::Api();
        if (!api || !api->log) return;
        char line[1024];
        std::vsnprintf(line, sizeof(line), format, args);
        api->log(category, line);
    }

    inline void LogCat(const char* category, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        LogCatV(category, format, args);
        va_end(args);
    }

    inline bool LogThrottle(const char* key, unsigned first = 8, unsigned every = 1000)
    {
        const WxlApi* api = module::Api();
        return api && api->logThrottle && api->logThrottle(key, first, every) != 0;
    }

    inline const char* WorkingDirectory()
    {
        const WxlApi* api = module::Api();
        return api && WXL_API_HAS(api, workingDirectory) && api->workingDirectory ? api->workingDirectory() : "";
    }
}

namespace wxl::storage
{
    /// A whole file through the client's reader (archives, then loose files under Data\). A miss is logged,
    /// rate-limited, the way the core's own ReadNative does.
    inline bool ReadNative(const char* name, std::vector<uint8_t>& out)
    {
        const WxlApi* api = module::Api();
        if (!api || !api->readFile || !api->freeBuffer || !name) return false;
        WxlBuffer buf = {};
        if (!api->readFile(name, &buf) || !buf.data)
        {
            if (wxl::LogThrottle("storage.readfail", 60, 200))
                wxl::LogCat("STORAGE", "ReadNative('%s') -> not found or unreadable", name);
            return false;
        }
        out.assign(static_cast<const uint8_t*>(buf.data), static_cast<const uint8_t*>(buf.data) + buf.size);
        api->freeBuffer(&buf);
        return true;
    }
}

// ---------------------------------------------------------------------------------------------------------
namespace wxl::events
{
    /// The events a module can bind with EventScript::on<>. Names are what the core knows them by.
    enum class Event : uint32_t { OnModelLoadPre, OnModelLoad, OnBuildBonePalette, OnUpdate, OnWorldEnter, OnWorldLeave };

    inline const char* EventName(Event e)
    {
        switch (e)
        {
            case Event::OnModelLoadPre:    return WXL_EVENT_MODEL_LOAD_PRE;
            case Event::OnModelLoad:       return WXL_EVENT_MODEL_LOAD;
            case Event::OnBuildBonePalette:return WXL_EVENT_BUILD_BONE_PALETTE;
            case Event::OnUpdate:          return WXL_EVENT_UPDATE;
            case Event::OnWorldEnter:      return WXL_EVENT_WORLD_ENTER;
            case Event::OnWorldLeave:      return WXL_EVENT_WORLD_LEAVE;
        }
        return "";
    }

    struct ModelLoadArgs         { void* model; };
    struct BuildBonePaletteArgs  { void* instance; };
    struct UpdateArgs            { float dt; uint32_t timeMs; };
    struct WorldArgs             { uint32_t mapId; };
    static_assert(sizeof(ModelLoadArgs) == sizeof(WxlModelLoadArgs) && sizeof(BuildBonePaletteArgs) == sizeof(WxlBuildBonePaletteArgs) &&
                  sizeof(UpdateArgs) == sizeof(WxlUpdateArgs) && sizeof(WorldArgs) == sizeof(WxlWorldArgs), "args mirror the C structs");

    namespace detail
    {
        template <class C, class A> struct MemArg { using Class = C; using Arg = A; };
        template <class C, class A> MemArg<C, A> MemArgOf(void (C::*)(const A&));
    }

    /// Base class a module's extension subclasses to bind member functions as event handlers.
    class EventScript
    {
    protected:
        EventScript() = default;
        virtual ~EventScript() = default;

        template <auto Method>
        void on(Event e)
        {
            using Traits = decltype(detail::MemArgOf(Method));
            using Self   = typename Traits::Class;
            using Arg    = typename Traits::Arg;
            const WxlApi* api = module::Api();
            if (!api || !api->subscribe) return;
            api->subscribe(EventName(e), [](void* user, const void* args) {
                Self* self = static_cast<Self*>(static_cast<EventScript*>(user));
                (self->*Method)(*static_cast<const Arg*>(args));
            }, this);
        }
    };
}

// ---------------------------------------------------------------------------------------------------------
// The virtual-path functions and ini helpers, under the namespace the in-tree code reaches them through.
namespace wxl::scripts::equipextension
{
    inline bool WxlIniGetBool(const char* section, const char* key, bool defaultValue)
    {
        const WxlApi* api = module::Api();
        return api && api->iniGetBool ? api->iniGetBool(section, key, defaultValue ? 1 : 0) != 0 : defaultValue;
    }
    inline int WxlIniGetInt(const char* section, const char* key, int defaultValue)
    {
        const WxlApi* api = module::Api();
        return api && api->iniGetInt ? api->iniGetInt(section, key, defaultValue) : defaultValue;
    }
    inline bool WxlIniGetString(const char* section, const char* key, char* out, size_t cap)
    {
        if (out && cap) out[0] = '\0';
        const WxlApi* api = module::Api();
        return api && api->iniGetString && api->iniGetString(section, key, out, cap) != 0;
    }
    inline uint64_t WxlIniStamp()
    {
        const WxlApi* api = module::Api();
        return api && api->iniStamp ? api->iniStamp() : 0;
    }

    inline bool VPathIsVirtualKey(const char* name)
    {
        const WxlApi* api = module::Api();
        return api && api->isVirtualKey && api->isVirtualKey(name) != 0;
    }

    inline bool VPathPopulate(void* cmo, const char* realMdxPath, const uint16_t* geoIds, uint32_t geoCount, const char* texPath,
                              uint32_t variantKey = 0, const char* materialPatchSpec = nullptr)
    {
        const WxlApi* api = module::Api();
        return WXL_API_HAS(api, vpathPopulate) && api->vpathPopulate &&
               api->vpathPopulate(cmo, realMdxPath, geoIds, geoCount, texPath, variantKey, materialPatchSpec) != 0;
    }

    inline void VPathEvictCmo(void* cmo)
    {
        const WxlApi* api = module::Api();
        if (WXL_API_HAS(api, vpathEvictCmo) && api->vpathEvictCmo) api->vpathEvictCmo(cmo);
    }

    inline bool VPathPopulateGlobal(const char* realMdxPath, uint32_t itemDisplayId, const char* texPath,
                                    const char* materialPatchSpec, const uint16_t* geoIds = nullptr, uint32_t geoCount = 0,
                                    bool evictable = false, char* outVirtualPath = nullptr, size_t outVirtualPathSz = 0,
                                    const char* evictionPool = "Creature", bool forceBake = false, bool mirrorAcrossY = false,
                                    const char* keySuffix = nullptr)
    {
        const WxlApi* api = module::Api();
        if (!WXL_API_HAS(api, vpathPopulateGlobal) || !api->vpathPopulateGlobal) return false;
        WxlVPathGlobal p = {};
        p.structSize = sizeof(p);
        p.realMdxPath = realMdxPath;
        p.itemDisplayId = itemDisplayId;
        p.texPath = texPath;
        p.materialPatchSpec = materialPatchSpec;
        p.geoIds = geoIds;
        p.geoCount = geoCount;
        p.evictable = evictable ? 1 : 0;
        p.outVirtualPath = outVirtualPath;
        p.outVirtualPathSz = outVirtualPathSz;
        p.evictionPool = evictionPool;
        p.forceBake = forceBake ? 1 : 0;
        p.mirrorAcrossY = mirrorAcrossY ? 1 : 0;
        p.keySuffix = keySuffix;
        return api->vpathPopulateGlobal(&p) != 0;
    }

    inline bool VPathPopulateGlobalSharedKey(const char* keySourcePath, const char* byteSourcePath, uint32_t itemDisplayId,
                                             const char* texPath, const char* materialPatchSpec, const uint16_t* geoIds,
                                             uint32_t geoCount, char* outVirtualPath, size_t outVirtualPathSz)
    {
        const WxlApi* api = module::Api();
        return WXL_API_HAS(api, vpathPopulateGlobalSharedKey) && api->vpathPopulateGlobalSharedKey &&
               api->vpathPopulateGlobalSharedKey(keySourcePath, byteSourcePath, itemDisplayId, texPath, materialPatchSpec, geoIds,
                                                 geoCount, outVirtualPath, outVirtualPathSz) != 0;
    }

    inline bool VPathDecodeGlobalVirtualKey(const char* virtualPathName, char* outNormPath, size_t outNormPathSz,
                                            uint32_t* outDisplayId)
    {
        const WxlApi* api = module::Api();
        return WXL_API_HAS(api, vpathDecodeGlobalVirtualKey) && api->vpathDecodeGlobalVirtualKey &&
               api->vpathDecodeGlobalVirtualKey(virtualPathName, outNormPath, outNormPathSz, outDisplayId) != 0;
    }

    using VPathLazyResolver = bool (*)(const char* normalizedVirtualPathName);

    namespace detail
    {
        // The table's resolver has no user pointer, so a bool-returning C++ resolver is reached through one of a
        // few trampolines, each bound to one registered function. A module registers one or two.
        constexpr size_t kMaxResolvers = 4;
        inline VPathLazyResolver* ResolverSlots() { static VPathLazyResolver slots[kMaxResolvers] = {}; return slots; }
        template <size_t N> int ResolverTrampoline(const char* name) { return ResolverSlots()[N] && ResolverSlots()[N](name) ? 1 : 0; }
        template <size_t... I> std::array<WxlVPathLazyResolver, sizeof...(I)> MakeResolverTrampolines(std::index_sequence<I...>)
        {
            return { { &ResolverTrampoline<I>... } };
        }
    }

    inline void VPathRegisterLazyResolver(VPathLazyResolver resolver)
    {
        const WxlApi* api = module::Api();
        if (!resolver || !WXL_API_HAS(api, vpathRegisterLazyResolver) || !api->vpathRegisterLazyResolver) return;
        static size_t used = 0;
        static const auto tramps = detail::MakeResolverTrampolines(std::make_index_sequence<detail::kMaxResolvers>());
        if (used >= detail::kMaxResolvers) return;
        detail::ResolverSlots()[used] = resolver;
        api->vpathRegisterLazyResolver(tramps[used]);
        ++used;
    }
}
