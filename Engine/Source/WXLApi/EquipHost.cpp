// The core's side of the engine-access members of the module API. See EquipHost.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The code here is what the equip extension did to the engine while it was compiled into the client (it was
// Engine/Source/WXLEquip/EquipExtension.cpp and EngineAccess.h): the guarded reads, the palette copy and mirror, the
// skin triangle filter, the DB record reads. It moved here unchanged in behaviour so that the extension, now a module,
// needs no engine header.

#include "Engine/Source/WXLApi/EquipHost.h"

#ifndef WXL_ALLOW_32BIT_HEADER   // the module API is 64-bit only: a 32-bit client has only the declarations

#include "Engine/Source/WXL/HandItem.h"
#include "Engine/Source/WXL/Log.h"
#include "Engine/Source/WXLEquip/EngineAccess.h"
#include "Engine/Source/WXLEquip/VirtualPath.hpp"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/ChrRacesRec.h"
#include "WoW/Source/DB/Rec/ItemDisplayInfoRec.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace wxl::api::equiphost
{
    namespace eng = wxl::equip::eng;

    namespace
    {
        constexpr uint32_t kMaxBones = 65535;

        void LogEquip(const char* format, ...) noexcept
        {
            va_list args;
            va_start(args, format);
            wxl::LogCatV("EQUIP", format, args);
            va_end(args);
        }

        // Plain strings out of the DBC string table: a pointer that is not plausible is an empty string.
        void CopyText(char* dst, size_t cap, const char* src) noexcept
        {
            if (!dst || cap == 0) return;
            dst[0] = '\0';
            if (!src || reinterpret_cast<uintptr_t>(src) <= 0x10000) return;
            std::strncpy(dst, src, cap - 1);
            dst[cap - 1] = '\0';
        }

        // ---- no C++ objects in a function with __try: each guarded step is its own tiny function ----

        bool SafeModelInfo(void* instance, WxlModelInfo* out) noexcept
        {
            __try
            {
                CM2Model* m = static_cast<CM2Model*>(instance);
                out->flags   = m->m_fFlags;
                out->scene   = m->m_scene;
                out->parent  = m->m_attachParent;
                out->palette = m->m_boneMatrices;
                CM2Shared* shared = m->m_shared;
                out->shared  = shared;
                if (shared)
                {
                    const M2Data* header = shared->m_data;
                    out->parsed = ((shared->m_flags & 1u) != 0 && header) ? 1u : 0u;
                    if (header)
                    {
                        const uint32_t count = header->m_bones.m_count;
                        out->boneCount = count <= kMaxBones ? count : 0u;
                    }
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        int SafeReadBoneInfo(void* instance, WxlBoneRead* io) noexcept
        {
            __try
            {
                CM2Shared* shared = static_cast<CM2Model*>(instance)->m_shared;
                if (!shared) return WXL_READ_NO_MODEL;
                if (!((shared->m_flags & 1u) != 0 && shared->m_data)) return WXL_READ_NOT_PARSED;
                const M2Data* header = shared->m_data;

                const uint32_t boneCount = header->m_bones.m_count;
                io->boneCount = boneCount;
                const M2CompBone* bones = boneCount ? header->m_bones.Data() : nullptr;
                if (bones)
                {
                    io->bonesOk = 1;
                    const uint32_t n = boneCount < io->maxBones ? boneCount : io->maxBones;
                    for (uint32_t i = 0; io->bones && i < n; ++i)
                    {
                        io->bones[i].nameCrc = bones[i].m_boneNameCRC;
                        io->bones[i].boneId  = static_cast<int32_t>(bones[i].m_boneId);
                        io->bones[i].parent  = static_cast<int32_t>(static_cast<int16_t>(bones[i].m_parentIndex));
                    }
                }

                const uint32_t lookupCount = header->m_boneIndicesById.m_count;
                io->lookupCount = lookupCount;
                const uint16_t* lookup = lookupCount ? header->m_boneIndicesById.Data() : nullptr;
                if (lookup)
                {
                    io->lookupOk = 1;
                    const uint32_t n = lookupCount < io->maxLookup ? lookupCount : io->maxLookup;
                    for (uint32_t i = 0; io->lookup && i < n; ++i) io->lookup[i] = lookup[i];
                }
                return WXL_READ_OK;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return WXL_READ_NO_HEADER; }
        }

        bool SafeCopyPath(void* shared, char* out, size_t cap) noexcept
        {
            __try
            {
                const char* name = static_cast<CM2Shared*>(shared)->m_fileName;
                if (!name || static_cast<unsigned char>(name[0]) < 0x20 || static_cast<unsigned char>(name[0]) > 0x7E) return false;
                std::strncpy(out, name, cap - 1);
                out[cap - 1] = '\0';
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void* SafeSkin(void* shared) noexcept
        {
            __try { return static_cast<CM2Shared*>(shared)->m_skinData; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        }

        // The triangle filter of the injected build: every triangle of a submesh whose id is not listed is collapsed onto
        // one vertex, so the batch draws nothing. (This is the old ApplyRawTriFilter.)
        void SafeFilterSkin(M2SkinProfile* skin, const uint16_t* ids, uint32_t idCount) noexcept
        {
            __try
            {
                const uint32_t indexCount   = skin->m_indices.m_count;
                const uint32_t submeshCount = skin->m_skinSections.m_count;
                if (indexCount == 0 || submeshCount == 0) return;

                uint16_t*            indices = skin->m_indices.Data();
                const M2SkinSection* subs    = skin->m_skinSections.Data();

                LogEquip("  ApplyRawTriFilter: submeshCount=%u indexCount=%u filter=[%u %u %u %u](n=%u)", submeshCount,
                         indexCount, idCount > 0 ? ids[0] : 0, idCount > 1 ? ids[1] : 0, idCount > 2 ? ids[2] : 0,
                         idCount > 3 ? ids[3] : 0, idCount);
                for (uint32_t si = 0; si < submeshCount; ++si)
                {
                    const uint16_t secId   = static_cast<uint16_t>(subs[si].m_skinSectionId & 0xFFFF);
                    const uint16_t lvl     = static_cast<uint16_t>(subs[si].m_skinSectionId >> 16);
                    const uint16_t start16 = subs[si].m_indexStart;
                    const uint16_t count16 = subs[si].m_indexCount;
                    bool visible = false;
                    for (uint32_t fi = 0; fi < idCount; ++fi)
                        if (ids[fi] == secId) { visible = true; break; }
                    LogEquip("    submesh[%u]: skinSectionId=%u level=%u indexStart=%u indexCount=%u -> %s", si,
                             (uint32_t)secId, (uint32_t)lvl, (uint32_t)start16, (uint32_t)count16, visible ? "KEEP" : "ZERO");
                    if (!visible)
                    {
                        uint32_t count = count16;
                        if (count == 0) continue;
                        // the index start can carry a high half in the section id's upper bits
                        uint32_t start = (static_cast<uint32_t>(lvl) << 16) | start16;
                        if (start > indexCount || count > indexCount - start)
                        {
                            start = start16;
                            if (start > indexCount || count > indexCount - start) continue;
                        }
                        const uint16_t fill = subs[si].m_vertexCount ? subs[si].m_vertexStart : static_cast<uint16_t>(0);
                        for (uint32_t k = 0; k < count; ++k) indices[start + k] = fill;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        bool SafeCmoInfo(void* cmo, WxlCmoInfo* out) noexcept
        {
            __try
            {
                out->model  = eng::CmoModel(cmo);
                out->race   = eng::CmoRace(cmo);
                out->gender = eng::CmoGender(cmo);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void* SafeCreateModel(void* scene, const char* path) noexcept
        {
            __try { return eng::GetRenderCtx(scene, path); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  createModel CRASH CAUGHT scene=%p path='%s'", scene, path ? path : "(null)"); return nullptr; }
        }

        void SafeAttach(void* child, void* parent, uint32_t attachId, int force) noexcept
        {
            __try { eng::AttachToScene(child, parent, attachId, force != 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  attachModel CRASH CAUGHT child=%p parent=%p attachId=%u", child, parent, attachId); }
        }

        void SafeDetach(void* parent, uint32_t attachId) noexcept
        {
            __try { eng::DetachSlot(parent, attachId); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  DetachSlot CRASH CAUGHT attachId=%u subObj=%p", attachId, parent); }
        }

        void SafeRelease(void* instance) noexcept
        {
            __try { eng::ReleaseRenderCtx(instance); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  releaseModel CRASH CAUGHT instance=%p", instance); }
        }

        void SafeBind(void* instance, void* texture) noexcept
        {
            __try { eng::BindTexSlot(instance, texture); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  BindTexSlot CRASH CAUGHT rctx=%p tex=%p", instance, texture); }
        }

        void* SafeLoadTexture(const char* path) noexcept
        {
            __try { return eng::LoadResource(path); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  LoadResource CRASH CAUGHT tex='%s'", path ? path : "(null)"); return nullptr; }
        }

        // "dir\name.m2" and "other\NAME.m2" are the same model: compare the lower-cased file stems.
        bool NameStemEqualsNoCase(const char* a, const char* b) noexcept
        {
            auto stem = [](const char* p, char* out, size_t cap) {
                const char* last = p;
                for (const char* c = p; *c; ++c) if (*c == '\\' || *c == '/') last = c + 1;
                size_t n = 0;
                for (const char* c = last; *c && n + 1 < cap; ++c) out[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
                out[n] = 0;
                char* dot = std::strrchr(out, '.');
                if (dot) *dot = 0;
            };
            char sa[260], sb[260];
            stem(a, sa, sizeof(sa));
            stem(b, sb, sizeof(sb));
            return sa[0] && std::strcmp(sa, sb) == 0;
        }

        void* SafeFindAttachedChild(void* parent, uint32_t attachId, const char* name) noexcept
        {
            __try
            {
                uint32_t guard = 0;
                for (CM2Model* c = static_cast<CM2Model*>(parent)->m_attachList; c && guard < 256; c = c->m_attachNext, ++guard)
                {
                    if (c->m_attachmentId != attachId || !c->m_shared) continue;
                    if (NameStemEqualsNoCase(c->m_shared->m_fileName, name)) return c;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return nullptr;
        }

        bool SafeHasSequence(void* instance, uint32_t id) noexcept
        {
            __try { return eng::HasSequence(instance, id); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        uint32_t SafeBoneSequenceId(void* instance, uint32_t boneId) noexcept
        {
            __try { return eng::GetBoneSequenceId(instance, boneId); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
        }

        void SafeSetBoneSequence(void* instance, uint32_t boneId, uint32_t sequenceId, uint32_t variation, uint32_t time,
                                 float speed, int32_t blend, int32_t primary) noexcept
        {
            __try { eng::SetBoneSequence(instance, boneId, sequenceId, variation, time, speed, blend, primary); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LogEquip("  setBoneSequence CRASH CAUGHT instance=%p seq=%u", instance, sequenceId); }
        }

        int32_t SafeSeqStartAnchor(void* instance) noexcept
        {
            __try { return eng::SeqStartAnchor(instance); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        uint32_t BoneCountOf(void* instance) noexcept
        {
            __try
            {
                const M2Data* header = eng::ModelHeader(instance);
                if (!header) return 0;
                const uint32_t count = header->m_bones.m_count;
                return count <= kMaxBones ? count : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        // The palette is allocated as modelBoneCount * 0x40 by the engine. A stale instance address can be recycled for a
        // smaller model while the caller still holds the old table; an unguarded copy loop would then write beyond the new
        // palette and corrupt the heap. So: both instances must still hold the models the table was built for, and the
        // sizes must match, before one row moves.
        bool SafeCopyBones(void* dst, void* src, const uint8_t* collToChar, uint32_t count, const void* dstShared,
                           const void* srcShared) noexcept
        {
            const uint32_t dstBoneCount = BoneCountOf(dst);
            const uint32_t srcBoneCount = BoneCountOf(src);
            __try
            {
                uint8_t*       dstBuf = eng::ModelBonePalette(dst);
                const uint8_t* srcBuf = eng::ModelBonePalette(src);
                if (!dstBuf || !srcBuf || dstBuf == srcBuf || !collToChar || count == 0 || count > kMaxBones ||
                    dstBoneCount == 0 || srcBoneCount == 0 || count != dstBoneCount || srcBoneCount > kMaxBones ||
                    !dstShared || !srcShared || eng::ModelShared(dst) != dstShared || eng::ModelShared(src) != srcShared)
                    return false;

                for (uint32_t bi = 0; bi < count; ++bi)
                {
                    const uint8_t ci = collToChar[bi];
                    if (ci == 0xFF) continue;
                    if (ci >= srcBoneCount) continue;
                    std::memcpy(dstBuf + bi * eng::kBonePaletteStride, srcBuf + ci * eng::kBonePaletteStride,
                                eng::kBonePaletteStride);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            return true;
        }

        // Returns true if the palette was (re)built by the engine during THIS frame. OnBuildBonePalette fires on EXIT of
        // CM2Model::AnimateMT, including its early-return paths (animation data not ready, or m_frameStamp already equal
        // to the scene's frame stamp). On those paths the palette was NOT rebuilt, so mirroring again would un-mirror
        // (double flip) or touch a palette that was never filled.
        bool SafePaletteBuilt(void* instance, uint32_t* stampOut) noexcept
        {
            __try
            {
                auto* m = static_cast<CM2Model*>(instance);
                if (!m || !m->m_scene || !m->m_shared || !m->m_shared->m_data) return false;
                if (!(m->m_fFlags & 1)) return false;           // animation data not ready yet
                if (!m->m_boneMatrices) return false;
                *stampOut = m->m_scene->m_frameStamp;
                return m->m_frameStamp == *stampOut;            // built this frame
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Mirrors the weapon across the model's local Y axis, bone by bone, on the freshly built palette.
        //
        // A palette matrix maps a model-space vertex to view space: M_b = L_b * W, where L_b is the bone's posed local
        // transform and W = m_matrixF4 (world * view). The correct mirrored model is the posed model reflected in model
        // space: v' = (v * L_b) * S * W, S = diag(1,-1,1). That is NOT S * M_b (reflect the vertex first): that only holds
        // for bones lying on the mirror plane, and displaced every bone whose pivot has y != 0. So:
        //
        //   regular bone   : M_b' = M_b * Q,  Q = W^-1 * S * W      (one Q per model per frame)
        //
        //   billboard bone : the engine rebuilds these from the CAMERA every frame (rotation rows replaced by view-aligned
        //                    ones, translation re-pinned so the bone pivot p stays at pivot_view = p * M_b). Applying Q to
        //                    the rotation would turn the quad away from the camera. Instead: reflect the quad in its LOCAL
        //                    Y about its own pivot (negate row b, and d += 2*p.y*rowB_old -- this keeps p fixed), so it
        //                    still faces the camera but has the reversed winding the swapped cull mode expects, then move
        //                    the pivot to where the mirrored skeleton puts it: pivot_view * Q.
        //
        // Billboard test is the engine's own (CM2Model::AnimateMT): (boneData->m_flags | bone->m_flags) & 0x78.
        void MirrorPaletteImpl(CM2Model* m, uint32_t boneCount) noexcept
        {
            M2Data* data = m->m_shared->m_data;
            const C44Matrix W = m->m_matrixF4;
            const bool haveQ = std::fabs(W.Determinant()) > 1e-12f;
            C44Matrix Q;
            Q.Identity();
            if (haveQ)
            {
                C44Matrix S;
                S.Identity();
                S.b1 = -1.0f;
                Q = W.Inverse() * S * W;
            }

            for (uint32_t bi = 0; bi < boneCount; ++bi)
            {
                C44Matrix& M = m->m_boneMatrices[bi];
                const uint32_t flags = data->m_bones[bi].m_flags | (m->m_bones ? m->m_bones[bi].m_flags : 0u);

                if (flags & 0x78) // billboard bone
                {
                    const C3Vector p = data->m_bones[bi].m_pivot;
                    const C3Vector pv = p * M;                         // where the engine pinned the pivot
                    const C3Vector pq = haveQ ? (pv * Q) : pv;         // where the mirrored skeleton needs it
                    const float ob0 = M.b0, ob1 = M.b1, ob2 = M.b2;
                    M.b0 = -ob0; M.b1 = -ob1; M.b2 = -ob2;             // local-Y reflection (keeps camera facing)
                    M.d0 += 2.0f * p.y * ob0 + (pq.x - pv.x);
                    M.d1 += 2.0f * p.y * ob1 + (pq.y - pv.y);
                    M.d2 += 2.0f * p.y * ob2 + (pq.z - pv.z);
                }
                else if (haveQ)
                {
                    M = M * Q;
                }
                else
                {
                    M.b0 = -M.b0; M.b1 = -M.b1; M.b2 = -M.b2; M.b3 = -M.b3; // degenerate world matrix fallback
                }
            }
        }

        bool SafeMirror(void* instance) noexcept
        {
            const uint32_t boneCount = BoneCountOf(instance);
            if (boneCount == 0) return false;
            __try
            {
                auto* m = static_cast<CM2Model*>(instance);
                if (!m->m_boneMatrices || !m->m_shared || !m->m_shared->m_data) return false;
                MirrorPaletteImpl(m, boneCount);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        uint32_t SafeReadU32(const void* ptr) noexcept
        {
            uint32_t v = 0;
            __try { v = *static_cast<const uint32_t*>(ptr); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return v;
        }

        // The client asks the held-item hooks through C++ function pointers; the module's are C functions.
        WxlHandItemResolverFn   g_handResolver  = nullptr;
        WxlMirroredModelQueryFn g_mirrorQuery   = nullptr;

        bool HandItemTrampoline(uint32_t displayId, uint32_t invSlot, char* out, size_t outSize)
        {
            const WxlHandItemResolverFn fn = g_handResolver;
            return fn && fn(displayId, invSlot, out, outSize) != 0;
        }

        bool MirrorQueryTrampoline(const void* model)
        {
            const WxlMirroredModelQueryFn fn = g_mirrorQuery;
            return fn && fn(model) != 0;
        }
    }

    int ModelInfo(void* instance, WxlModelInfo* out)
    {
        if (!out || out->structSize < sizeof(WxlModelInfo)) return WXL_READ_NO_MODEL;
        out->flags = out->boneCount = out->parsed = 0;
        out->shared = out->scene = out->parent = nullptr;
        out->palette = nullptr;
        if (!instance) return WXL_READ_NO_MODEL;
        if (!SafeModelInfo(instance, out))
        {
            out->flags = out->boneCount = out->parsed = 0;
            out->shared = out->scene = out->parent = nullptr;
            out->palette = nullptr;
            return WXL_READ_NO_MODEL;
        }
        return WXL_READ_OK;
    }

    int ReadBoneInfo(void* instance, WxlBoneRead* io)
    {
        if (!io || io->structSize < sizeof(WxlBoneRead)) return WXL_READ_NO_MODEL;
        io->boneCount = io->lookupCount = io->bonesOk = io->lookupOk = 0;
        if (!instance) return WXL_READ_NO_MODEL;
        return SafeReadBoneInfo(instance, io);
    }

    size_t SharedPath(void* shared, char* out, size_t cap)
    {
        if (!out || cap == 0) return 0;
        out[0] = '\0';
        if (!shared || !SafeCopyPath(shared, out, cap)) { out[0] = '\0'; return 0; }
        return std::strlen(out);
    }

    void* SharedSkin(void* shared) { return shared ? SafeSkin(shared) : nullptr; }

    void FilterSkin(void* skin, const uint16_t* geosetIds, uint32_t count)
    {
        if (!skin || !geosetIds || count == 0) return;
        SafeFilterSkin(static_cast<M2SkinProfile*>(skin), geosetIds, count);
    }

    int CmoInfo(void* cmo, WxlCmoInfo* out)
    {
        if (!out || out->structSize < sizeof(WxlCmoInfo)) return WXL_READ_NO_MODEL;
        out->race = out->gender = out->reserved = 0;
        out->model = nullptr;
        if (!cmo) return WXL_READ_NO_MODEL;
        if (!SafeCmoInfo(cmo, out)) { out->race = out->gender = 0; out->model = nullptr; return WXL_READ_NO_MODEL; }
        return WXL_READ_OK;
    }

    void* CreateModel(void* scene, const char* path) { return scene && path ? SafeCreateModel(scene, path) : nullptr; }
    void  AttachModel(void* child, void* parent, uint32_t attachId, int force) { if (child && parent) SafeAttach(child, parent, attachId, force); }
    void  DetachSlot(void* parent, uint32_t attachId) { if (parent) SafeDetach(parent, attachId); }
    void  ReleaseModel(void* instance) { if (instance) SafeRelease(instance); }
    void  BindTexture(void* instance, void* texture) { if (instance) SafeBind(instance, texture); }
    void* LoadTexture(const char* path) { return path ? SafeLoadTexture(path) : nullptr; }
    void* FindAttachedChild(void* parent, uint32_t attachId, const char* name) { return parent && name ? SafeFindAttachedChild(parent, attachId, name) : nullptr; }

    int      HasSequence(void* instance, uint32_t sequenceId) { return instance && SafeHasSequence(instance, sequenceId) ? 1 : 0; }
    uint32_t BoneSequenceId(void* instance, uint32_t boneId) { return instance ? SafeBoneSequenceId(instance, boneId) : 0xFFFFFFFFu; }
    void     SetBoneSequence(void* instance, uint32_t boneId, uint32_t sequenceId, uint32_t variation, uint32_t time,
                             float speed, int32_t blend, int32_t primary)
    {
        if (instance) SafeSetBoneSequence(instance, boneId, sequenceId, variation, time, speed, blend, primary);
    }
    int32_t SeqStartAnchor(void* instance) { return instance ? SafeSeqStartAnchor(instance) : 0; }

    int CopyRemappedBones(void* dst, void* src, const uint8_t* collToChar, uint32_t count, const void* dstShared,
                          const void* srcShared)
    {
        return dst && src && SafeCopyBones(dst, src, collToChar, count, dstShared, srcShared) ? 1 : 0;
    }

    int PaletteBuiltThisFrame(void* instance, uint32_t* stamp)
    {
        uint32_t local = 0;
        const bool built = instance && SafePaletteBuilt(instance, &local);
        if (stamp) *stamp = local;
        return built ? 1 : 0;
    }

    int MirrorPalette(void* instance) { return instance && SafeMirror(instance) ? 1 : 0; }

    int ItemDisplayInfo(uint32_t displayId, WxlItemDisplayInfo* out)
    {
        if (!out || out->structSize < sizeof(WxlItemDisplayInfo)) return 0;
        out->modelName[0][0] = out->modelName[1][0] = out->modelTexture[0][0] = out->modelTexture[1][0] = out->icon2[0] = '\0';
        ItemDisplayInfoRec rec;
        if (!g_itemDisplayInfoDB.GetRecord(static_cast<int32_t>(displayId), &rec)) return 0;
        CopyText(out->modelName[0], WXL_ITEM_TEXT, rec.m_modelName[0]);
        CopyText(out->modelName[1], WXL_ITEM_TEXT, rec.m_modelName[1]);
        CopyText(out->modelTexture[0], WXL_ITEM_TEXT, rec.m_modelTexture[0]);
        CopyText(out->modelTexture[1], WXL_ITEM_TEXT, rec.m_modelTexture[1]);
        CopyText(out->icon2, WXL_ITEM_TEXT, rec.m_inventoryIcon[1]);
        return 1;
    }

    int RaceClientPrefix(uint32_t raceId, char* out, size_t cap, int32_t* maxRaceId)
    {
        if (out && cap) out[0] = '\0';
        const int32_t maxId = g_chrRacesDB.m_maxID;
        if (maxRaceId) *maxRaceId = maxId;
        if (static_cast<int32_t>(raceId) < 0 || static_cast<int32_t>(raceId) > maxId) return 1;
        ChrRacesRec* record = g_chrRacesDB.GetRecord(static_cast<int32_t>(raceId));
        if (!record) return 2;
        const char* prefix = record->m_clientPrefix;
        if (!prefix) return 3;
        CopyText(out, cap, prefix);
        return 0;
    }

    uint32_t ItemDataDisplayId(const void* itemData) { return itemData ? SafeReadU32(itemData) : 0; }

    void SetHandItemModelResolver(WxlHandItemResolverFn fn)
    {
        g_handResolver = fn;
        wxl::hooks::SetHandItemModelResolver(fn ? &HandItemTrampoline : nullptr);
    }

    void SetMirroredModelQuery(WxlMirroredModelQueryFn fn)
    {
        g_mirrorQuery = fn;
        wxl::hooks::SetMirroredModelQuery(fn ? &MirrorQueryTrampoline : nullptr);
    }

    size_t VPathBuildKey(char* out, size_t outSz, void* cmo, const char* realMdxPath, const uint16_t* geoIds,
                         uint32_t geoCount, const char* texPath, uint32_t variantKey, const char* materialPatchSpec)
    {
        return wxl::scripts::equipextension::VPathBuildKey(out, outSz, cmo, realMdxPath, geoIds, geoCount, texPath, variantKey,
                                                           materialPatchSpec);
    }
}

#endif // !WXL_ALLOW_32BIT_HEADER
