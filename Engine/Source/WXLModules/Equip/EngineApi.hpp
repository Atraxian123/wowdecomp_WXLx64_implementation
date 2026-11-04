// The equip module's view of the client: every engine fact and engine call goes through the module API table
// (Engine/Source/WXL/Sdk/wxl_api.h, "engine access"), so this module needs no engine header. The names are the ones
// the extension used when it was compiled into the client (it called them in Engine/Source/WXLEquip/EngineAccess.h).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
#pragma once

#include "wxl_module.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wxl::equip::eng
{
    namespace detail
    {
        inline const WxlApi* Api() { return wxl::module::Api(); }
        inline bool Ready() { const WxlApi* api = Api(); return api && WXL_API_HAS(api, vpathBuildKey); }
    }

    // --- instances (CM2Model handles) ---

    inline bool Info(void* model, WxlModelInfo& out)
    {
        std::memset(&out, 0, sizeof(out));
        out.structSize = sizeof(out);
        return model && detail::Ready() && detail::Api()->modelInfo(model, &out) == 0;
    }
    inline void*    ModelScene(void* model)       { WxlModelInfo i; return Info(model, i) ? i.scene : nullptr; }
    inline uint32_t ModelFlags(void* model)       { WxlModelInfo i; return Info(model, i) ? i.flags : 0; }
    inline void*    ModelBonePalette(void* model) { WxlModelInfo i; return Info(model, i) ? const_cast<void*>(i.palette) : nullptr; }
    inline void*    ModelParent(void* model)      { WxlModelInfo i; return Info(model, i) ? i.parent : nullptr; }
    inline void*    ModelShared(void* model)      { WxlModelInfo i; return Info(model, i) ? i.shared : nullptr; }
    /// The bone tables of the instance's loaded model (see WxlBoneRead): 0 ok, WXL_READ_NOT_PARSED until it is fully loaded.
    inline int ReadBones(void* model, WxlBoneRead& io)
    {
        return model && detail::Ready() ? detail::Api()->readBoneInfo(model, &io) : static_cast<int>(WXL_READ_NO_MODEL);
    }
    /// Bones in the instance's loaded model; 0 when it is not loaded or the instance is gone.
    inline uint32_t ModelBoneCount(void* model)   { WxlModelInfo i; return Info(model, i) ? i.boneCount : 0; }

    // --- character components ---

    inline bool CmoInfoOf(void* cmo, WxlCmoInfo& out)
    {
        std::memset(&out, 0, sizeof(out));
        out.structSize = sizeof(out);
        return cmo && detail::Ready() && detail::Api()->cmoInfo(cmo, &out) == 0;
    }
    inline void*    CmoModel(void* cmo)  { WxlCmoInfo i; return CmoInfoOf(cmo, i) ? i.model : nullptr; }
    inline uint32_t CmoRace(void* cmo)   { WxlCmoInfo i; return CmoInfoOf(cmo, i) ? i.race : 0; }
    inline uint32_t CmoGender(void* cmo) { WxlCmoInfo i; return CmoInfoOf(cmo, i) ? i.gender : 0; }

    // --- loaded model files ---

    /// The model file's path as the cache keys it, in a per-thread buffer valid until the next call; NULL when unknown.
    inline const char* SharedPath(void* shared)
    {
        thread_local char buffer[260];
        buffer[0] = '\0';
        if (!shared || !detail::Ready() || detail::Api()->sharedPath(shared, buffer, sizeof(buffer)) == 0) return nullptr;
        return buffer;
    }
    inline void* SharedSkin(void* shared) { return shared && detail::Ready() ? detail::Api()->sharedSkin(shared) : nullptr; }
    inline void FilterSkin(void* skin, const uint16_t* ids, uint32_t count)
    {
        if (skin && detail::Ready()) detail::Api()->filterSkin(skin, ids, count);
    }

    // --- engine calls ---

    inline void* GetRenderCtx(void* scene, const char* path) { return detail::Ready() ? detail::Api()->createModel(scene, path) : nullptr; }
    inline void  AttachToScene(void* child, void* parent, uint32_t attachId, bool force)
    {
        if (detail::Ready()) detail::Api()->attachModel(child, parent, attachId, force ? 1 : 0);
    }
    inline void  DetachSlot(void* parent, uint32_t attachId) { if (detail::Ready()) detail::Api()->detachSlot(parent, attachId); }
    inline void  ReleaseRenderCtx(void* model)               { if (detail::Ready()) detail::Api()->releaseModel(model); }
    inline void  BindTexSlot(void* model, void* texture)     { if (detail::Ready()) detail::Api()->bindTexture(model, texture); }
    inline void* LoadResource(const char* path)              { return detail::Ready() ? detail::Api()->loadTexture(path) : nullptr; }
    inline void* FindAttachedChild(void* parent, uint32_t attachId, const char* name)
    {
        return detail::Ready() ? detail::Api()->findAttachedChild(parent, attachId, name) : nullptr;
    }

    // --- animation sequences ---

    inline bool     HasSequence(void* model, uint32_t sequenceId) { return detail::Ready() && detail::Api()->hasSequence(model, sequenceId) != 0; }
    inline uint32_t GetBoneSequenceId(void* model, uint32_t boneId) { return detail::Ready() ? detail::Api()->boneSequenceId(model, boneId) : 0xFFFFFFFFu; }
    inline void     SetBoneSequence(void* model, uint32_t boneId, uint32_t sequenceId, uint32_t variation, uint32_t time,
                                    float speed, int32_t blend, int32_t primary)
    {
        if (detail::Ready()) detail::Api()->setBoneSequence(model, boneId, sequenceId, variation, time, speed, blend, primary);
    }
    inline int32_t  SeqStartAnchor(void* model) { return detail::Ready() ? detail::Api()->seqStartAnchor(model) : 0; }

    // --- palettes ---

    inline bool CopyRemappedBones(void* dst, void* src, const uint8_t* collToChar, uint32_t count, const void* dstShared, const void* srcShared)
    {
        return detail::Ready() && detail::Api()->copyRemappedBones(dst, src, collToChar, count, dstShared, srcShared) != 0;
    }
    inline bool PaletteBuiltThisFrame(void* model, uint32_t& stamp)
    {
        stamp = 0;
        return detail::Ready() && detail::Api()->paletteBuiltThisFrame(model, &stamp) != 0;
    }
    inline bool MirrorPalette(void* model) { return detail::Ready() && detail::Api()->mirrorPalette(model) != 0; }

    // --- records ---

    inline bool ItemDisplayInfo(uint32_t displayId, WxlItemDisplayInfo& out)
    {
        std::memset(&out, 0, sizeof(out));
        out.structSize = sizeof(out);
        return detail::Ready() && detail::Api()->itemDisplayInfo(displayId, &out) != 0;
    }
    /// 0 ok, 1 race id out of range, 2 no record, 3 record without a prefix (see wxl_api.h).
    inline int RaceClientPrefix(uint32_t raceId, char* out, size_t cap, int32_t& maxRaceId)
    {
        maxRaceId = -1;
        if (out && cap) out[0] = '\0';
        return detail::Ready() ? detail::Api()->raceClientPrefix(raceId, out, cap, &maxRaceId) : 2;
    }
    inline uint32_t ItemDataDisplayId(const void* itemData) { return detail::Ready() ? detail::Api()->itemDataDisplayId(itemData) : 0; }

    // --- the client's held-item hooks ---

    inline void SetHandItemModelResolver(WxlHandItemResolverFn fn)  { if (detail::Ready()) detail::Api()->setHandItemModelResolver(fn); }
    inline void SetMirroredModelQuery(WxlMirroredModelQueryFn fn)   { if (detail::Ready()) detail::Api()->setMirroredModelQuery(fn); }
}

namespace wxl::scripts::equipextension
{
    inline size_t VPathBuildKey(char* out, size_t outSz, void* cmo, const char* realMdxPath, const uint16_t* geoIds,
                                uint32_t geoCount, const char* texPath, uint32_t variantKey, const char* materialPatchSpec)
    {
        return wxl::equip::eng::detail::Ready()
                   ? wxl::equip::eng::detail::Api()->vpathBuildKey(out, outSz, cmo, realMdxPath, geoIds, geoCount, texPath,
                                                                   variantKey, materialPatchSpec)
                   : 0;
    }
}
