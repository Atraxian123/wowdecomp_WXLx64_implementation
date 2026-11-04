// The core's side of the engine-access members of the module API (Engine/Source/WXL/Sdk/wxl_api.h): what the equip
// module calls to attach models, read bone tables, copy and mirror palettes, and read item and race records.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Like ModelAccess.cpp this is a place that knows what a CM2Model is. A module gets plain numbers and opaque handles;
// every read of client memory and every engine call that can meet a stale instance is made here under SEH.
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"

namespace wxl::api::equiphost
{
    int      ModelInfo(void* instance, WxlModelInfo* out);
    int      ReadBoneInfo(void* instance, WxlBoneRead* io);
    size_t   SharedPath(void* shared, char* out, size_t cap);
    void*    SharedSkin(void* shared);
    void     FilterSkin(void* skin, const uint16_t* geosetIds, uint32_t count);
    int      CmoInfo(void* cmo, WxlCmoInfo* out);

    void*    CreateModel(void* scene, const char* path);
    void     AttachModel(void* child, void* parent, uint32_t attachId, int force);
    void     DetachSlot(void* parent, uint32_t attachId);
    void     ReleaseModel(void* instance);
    void     BindTexture(void* instance, void* texture);
    void*    LoadTexture(const char* path);
    void*    FindAttachedChild(void* parent, uint32_t attachId, const char* name);

    int      HasSequence(void* instance, uint32_t sequenceId);
    uint32_t BoneSequenceId(void* instance, uint32_t boneId);
    void     SetBoneSequence(void* instance, uint32_t boneId, uint32_t sequenceId, uint32_t variation, uint32_t time,
                             float speed, int32_t blend, int32_t primary);
    int32_t  SeqStartAnchor(void* instance);

    int      CopyRemappedBones(void* dst, void* src, const uint8_t* collToChar, uint32_t count, const void* dstShared,
                               const void* srcShared);
    int      PaletteBuiltThisFrame(void* instance, uint32_t* stamp);
    int      MirrorPalette(void* instance);

    int      ItemDisplayInfo(uint32_t displayId, WxlItemDisplayInfo* out);
    int      RaceClientPrefix(uint32_t raceId, char* out, size_t cap, int32_t* maxRaceId);
    uint32_t ItemDataDisplayId(const void* itemData);

    void     SetHandItemModelResolver(WxlHandItemResolverFn fn);
    void     SetMirroredModelQuery(WxlMirroredModelQueryFn fn);

    size_t   VPathBuildKey(char* out, size_t outSz, void* cmo, const char* realMdxPath, const uint16_t* geoIds,
                           uint32_t geoCount, const char* texPath, uint32_t variantKey, const char* materialPatchSpec);
}
