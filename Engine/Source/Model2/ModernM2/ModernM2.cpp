// Modern M2 reading, the client's side: forwards the load steps to wxl_modernm2.dll (service "wxl.modernm2.v1").
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The decompiled loader (CM2Shared) calls the wxl::modernm2:: functions declared in ModernM2.h exactly where it always
// did. Each one checks the cheap things the client can know itself, finds the module's service, and passes it plain
// buffers plus the client's own allocators (WxlM2Alloc), so a block the module asks for is freed by the routine that
// allocated it. The per-model State (TXID table, reserve room, wide index starts) lives in CM2Shared::m_wxl and is
// owned here: the module keeps nothing between calls.

#include "Engine/Source/Model2/ModernM2/ModernM2.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

#include "Engine/Source/Base/SIMD.h"
#include "Engine/Source/Os/OsMemory.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXL/Services.h"

#ifndef WXL_ALLOW_32BIT_HEADER
#define WXL_ALLOW_32BIT_HEADER 1   // consumer side: only the table declaration is needed
#endif
#include "Engine/Source/WXL/Sdk/wxl_modernm2_api.h"

namespace wxl::modernm2
{
    namespace
    {
        /// MD21 container magic, as the little-endian dword of the on-disk bytes (not byte-reversed).
        constexpr uint32_t kTagMD21 = 0x3132444D;

        static_assert(kMaxTxid == WXL_M2_MAX_TXID, "the client's TXID cap and the module's must match");

        // ------------------------------------------------------------------------------- the service
        /// The module's table, or null while it is not loaded. Looked up at the time of the call (load order between
        /// the module and the first model does not matter) and cached once found; the table lives as long as the
        /// process.
        const WxlModernM2Services* Service()
        {
            static std::atomic<const WxlModernM2Services*> s_cached{ nullptr };
            const WxlModernM2Services* cached = s_cached.load(std::memory_order_acquire);
            if (cached) return cached;

            size_t size = 0;
            const auto* found = static_cast<const WxlModernM2Services*>(wxl::services::Get(WXL_SERVICE_MODERNM2, &size));
            if (!found || size < sizeof(uint32_t) || found->structSize < offsetof(WxlModernM2Services, unwrapAnim) + sizeof(found->unwrapAnim))
                return nullptr;

            if (found->versionMin != kVersionMin || found->versionMax != kVersionMax)
                wxl::LogCat("M2", "modern M2 module reads versions %u-%u but the client accepts %u-%u: rebuild both from the same tree",
                            found->versionMin, found->versionMax, kVersionMin, kVersionMax);
            s_cached.store(found, std::memory_order_release);
            return found;
        }

        // ------------------------------------------------------------------------ the client's allocators
        struct AllocContext
        {
            CM2Shared* shared;
        };

        State& EnsureState(CM2Shared* shared)
        {
            if (!shared->m_wxl) shared->m_wxl = new State();
            return *shared->m_wxl;
        }

        void* AllocBlock(void*, uint32_t size)
        {
            return SIMD::AllocA16(size, __FILE__, __LINE__);
        }

        void ReleaseBlock(void*, void* block)
        {
            if (block) SIMD::FreeA16(block);
        }

        /// A block within reach of a 32-bit offset from the model: owned by the model's State, released with it.
        void* AllocNearBlock(void* ctx, const void* anchor, size_t bytes)
        {
            CM2Shared* shared = static_cast<AllocContext*>(ctx)->shared;
#if defined(WOWDECOMP_ARCH_64)
            void* block = OsAllocNear(anchor, bytes);
#else
            (void)anchor;
            void* block = SMemAlloc(static_cast<uint32_t>(bytes), __FILE__, __LINE__, 0);
#endif
            if (block) EnsureState(shared).farBlocks.push_back(block);
            return block;
        }

        WxlM2Alloc MakeAlloc(AllocContext& context)
        {
            WxlM2Alloc mem = {};
            mem.structSize = sizeof(mem);
            mem.ctx        = &context;
            mem.alloc      = &AllocBlock;
            mem.release    = &ReleaseBlock;
            mem.allocNear  = &AllocNearBlock;
            return mem;
        }

        // ------------------------------------------------------------------------- the client's own fixes
        /// A section with boneInfluences == 0 takes the client's shadow-map vertex variant that skips the
        /// inverse(cameraView) * lightView rebase, which makes a shadow stay rigidly aligned to the camera
        /// instead of the light. Every drawn submesh with boneInfluences == 0 is lifted to 1. The module does this
        /// as part of its finalize; this copy covers a client running without it.
        void NormalizeBoneInfluences(M2SkinProfile* sk)
        {
            M2SkinSection* sections = sk->m_skinSections.Data();
            for (uint32_t i = 0; i < sk->m_skinSections.m_count; ++i)
                if (sections[i].m_indexCount != 0 && sections[i].m_boneInfluences == 0)
                    sections[i].m_boneInfluences = 1;
        }

        /// The shadow-pass animate step reads the shared's bone-flag tally (m_billboardBoneCount): at zero it
        /// takes a fast path that never rebuilds the bone palette, so the palette keeps an older frame's view
        /// matrix while the shadow renderer uploads the current one and the shadow drifts with the camera.
        /// Stock content always has the count > 0; a model from a toolchain that ships empty bone flags
        /// would not. Runtime object only, never the model data.
        void ForceShadowAnimGate(CM2Shared* shared)
        {
            if (shared->m_billboardBoneCount == 0) shared->m_billboardBoneCount = 1;
        }
    }

    bool ModuleLoaded() { return Service() != nullptr; }

    // ------------------------------------------------------------------------------------ PrepareLoad
    LoadResult PrepareLoad(CM2Shared* shared)
    {
        const uint8_t* buf = reinterpret_cast<const uint8_t*>(shared->m_data);
        const uint32_t size = shared->m_fileSize;
        uint32_t magic = 0;
        if (!buf || size < 8) return LoadResult::NotModern;
        std::memcpy(&magic, buf, sizeof(magic));
        if (magic != kTagMD21) return LoadResult::NotModern;   // a stock model: one compare, no service lookup

        const WxlModernM2Services* service = Service();
        if (!service)
        {
            if (wxl::LogThrottle("m2-nomodule", 1, 100000))
                wxl::LogCat("M2", "'%s' is a modern model but the module (%s) is not loaded: wxl_modernm2.dll missing from WXL\\Modules, or disabled",
                            shared->m_fileName, WXL_SERVICE_MODERNM2);
            return LoadResult::Failed;   // the stock walk cannot read an MD21 container: same as an absent file
        }

        AllocContext context{ shared };
        const WxlM2Alloc mem = MakeAlloc(context);
        WxlM2PrepareResult result = {};
        result.structSize = sizeof(result);

        const int status = service->prepareLoad(shared->m_fileName, buf, size, &mem, &result);
        if (status == WXL_M2_NOT_MODERN) return LoadResult::NotModern;
        if (status != WXL_M2_MODERN || !result.body) return LoadResult::Failed;

        // The module read the container and seated the flat body in a block of ours: swap it in. The model's block
        // is only ever freed through SIMD::FreeA16 on m_data, so the swap is invisible to the rest of the client.
        SIMD::FreeA16(shared->m_data);
        shared->m_data = static_cast<M2Data*>(result.body);
        shared->m_fileSize = result.bodySize;

        State* state = new (std::nothrow) State();
        if (!state)
        {
            wxl::LogCat("M2", "'%s' modern load FAILED: out of memory (state)", shared->m_fileName);
            return LoadResult::Failed;
        }
        state->modern = true;
        state->txidCount = result.txidCount > kMaxTxid ? kMaxTxid : result.txidCount;
        std::memcpy(state->txid, result.txid, sizeof(uint32_t) * state->txidCount);
        state->reserveBase = result.reserveBase;
        state->reserveCap = result.reserveCap;
        shared->m_wxl = state;
        return LoadResult::Modern;
    }

    // -------------------------------------------------------------------------------------- FinishWalk
    void FinishWalk(CM2Shared* shared)
    {
        const WxlModernM2Services* service = Service();
        if (service && shared->m_data) service->finishWalk(shared->m_data);
    }

    // ------------------------------------------------------------------------------------ TextureName
    bool TextureName(CM2Shared* shared, uint32_t index, const M2Texture& texture, char* out, size_t cap)
    {
        State* state = shared->m_wxl;
        if (!state || !state->modern) return false;
        if (texture.m_type != 0) return false;                  // dynamic slots stay empty
        if (index >= state->txidCount || !state->txid[index]) return false;

        const uint32_t fileId = state->txid[index];
        const WxlModernM2Services* service = Service();
        if (!service)
        {
            ++state->texUnresolved;
            return false;
        }

        // FOUND: the file's own path. NOTFOUND: the listfile module's not-found name for a texture; opening that name
        // makes the client fall back to its own missing-texture handling, which is the listfile module's contract.
        switch (service->textureName(fileId, out, cap))
        {
        case WXL_M2_TEXNAME_FOUND:    ++state->texResolved;   return true;
        case WXL_M2_TEXNAME_NOTFOUND: ++state->texUnresolved; return true;
        default:                      ++state->texUnresolved; return false;
        }
    }

    // ----------------------------------------------------------------------------------- FinalizeSkin
    void FinalizeSkin(CM2Shared* shared, uint32_t skinBytes)
    {
        M2Data* md = shared->m_data;
        M2SkinProfile* sk = shared->m_skinData;
        if (!md || !sk) return;

        const WxlModernM2Services* service = Service();
        bool ran = false;
        if (service)
        {
            AllocContext context{ shared };
            const WxlM2Alloc mem = MakeAlloc(context);

            const State* existing = shared->m_wxl;
            WxlM2SkinJob job = {};
            job.structSize  = sizeof(job);
            job.name        = shared->m_fileName;
            job.model       = md;
            job.skin        = sk;
            job.skinBytes   = skinBytes;
            job.modern      = (existing && existing->modern) ? 1u : 0u;
            if (existing)
            {
                job.reserveBase = existing->reserveBase;
                job.reserveCap  = existing->reserveCap;
                job.reserveUsed = existing->reserveUsed;
            }

            if (service->finalizeSkin(&mem, &job) == 0)
            {
                ran = true;
                shared->m_skinData = static_cast<M2SkinProfile*>(job.newSkin);   // re-seated when its arrays grew
                if (shared->m_wxl) shared->m_wxl->reserveUsed = job.reserveUsed;

                // Extended encoding: the full index starts the sections cannot hold. They are kept as the file has them
                // (sourceStart) and as a draw reads them (sectionStart, which the shared's index rebuild remaps later).
                if (job.extendedIndexStart && job.startCount && job.indexStart)
                {
                    State& state = EnsureState(shared);
                    state.extendedIndexStart = true;
                    state.sourceStart.assign(job.indexStart, job.indexStart + job.startCount);
                    state.sectionStart = state.sourceStart;
                }
            }
        }

        // No module (or it could not run): stock content still gets the one fix every model has always had.
        if (!ran) NormalizeBoneInfluences(shared->m_skinData);
        ForceShadowAnimGate(shared);
    }

    // -------------------------------------------------------------------------------------- UnwrapAnim
    bool UnwrapAnim(uint8_t* buffer, uint32_t& size)
    {
        // 'AFM2', read as the little-endian dword of the on-disk bytes. A stock animation file costs one compare.
        constexpr uint32_t kTagAFM2 = 0x324D4641;
        uint32_t magic = 0;
        if (!buffer || size < 8) return false;
        std::memcpy(&magic, buffer, sizeof(magic));
        if (magic != kTagAFM2) return false;

        const WxlModernM2Services* service = Service();
        return service && service->unwrapAnim(buffer, &size) != 0;
    }

    // ----------------------------------------------------------------------------------------- Destroy
    void Destroy(CM2Shared* shared)
    {
        State* state = shared->m_wxl;
        if (!state) return;
        shared->m_wxl = nullptr;
        for (void* block : state->farBlocks)
        {
#if defined(WOWDECOMP_ARCH_64)
            OsFreeNear(block);
#else
            SMemFree(block, __FILE__, __LINE__, 0);
#endif
        }
        delete state;
    }
}
