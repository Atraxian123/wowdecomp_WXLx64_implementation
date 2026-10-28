// Modern (MD21 container, inner version 272-274) M2 reader, compiled into the client.
// Ported from the wxl-modern-m2 extension (Copyright (C) 2026 WarcraftXL, GPLv3, see Engine/Source/WXL/Event.h).
//
// The extension reached the client from outside: detours on the stock loader, raw offsets turned into raw
// pointers. Here the same work sits at the decompiled load steps themselves and speaks the client's own
// record layout, in which every M2Array is a count plus a 32-bit offset RELATIVE TO THE ARRAY (so the
// stock M2Init walk is reused unchanged once the body has been reshaped).
//
// Load path (CM2Shared::FinishLoading):
//   PrepareLoad   MD21 container -> flat MD20 body in a freshly seated, slightly oversized block; in-place
//                 field deltas; particle / camera records brought to the client's record shape.
//   M2Init        the stock walk, which now accepts the modern version window.
//   FinishWalk    reference clamps on the walked header.
// Texture creation (CM2Shared::Initialize) asks TextureName for every hardcoded texture the file leaves
// unnamed: the TXID chunk's file id goes to the listfile module's service (wxl.listfile.v1).
// Skin load (CM2Shared::FinishLoadingSkinProfile) calls FinalizeSkin before the batch blocks are sized.
// External animation files go through UnwrapAnim.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Source/Model2/M2Shared.h"

namespace wxl::modernm2
{
    constexpr uint32_t kVersionMin = 272; // inner M2 version window the reader accepts
    constexpr uint32_t kVersionMax = 274;

    inline bool IsAcceptedVersion(uint32_t version) { return version >= kVersionMin && version <= kVersionMax; }

    constexpr uint32_t kMaxTxid = 128; // hard cap on the TXID entries kept per model

    /// The client's vertex shader holds the bone palette from register 31, 3 registers per matrix:
    /// (256 - 31) / 3 = 75 bones per draw.
    constexpr uint32_t kMaxBonesPerDraw = 75;

    /// Per-model state, owned by CM2Shared::m_wxl. Created only for models that need it.
    struct State
    {
        bool     modern = false;          // read through the MD21 path
        bool     extendedIndexStart = false; // skin sections carry (level << 16) | indexStart
        uint32_t txidCount = 0;
        uint32_t txid[kMaxTxid] = {};
        uint32_t texResolved = 0;
        uint32_t texUnresolved = 0;

        // Spare room at the end of the model block, where arrays the skin finalize rebuilds are placed
        // (an M2Array reaches its data by a signed 32-bit offset from itself, so it has to live close).
        uint32_t reserveBase = 0;
        uint32_t reserveCap = 0;
        uint32_t reserveUsed = 0;
        std::vector<void*> farBlocks;     // fallback allocations, released with the model

        // Full 32-bit index starts, kept when the model uses the extended encoding (the section record itself
        // only has 16 bits for it). sourceStart: where each section's indices sit in the skin file's index
        // array. sectionStart: where they sit in the shared's rebuilt index buffer (what a draw reads).
        std::vector<uint32_t> sourceStart;
        std::vector<uint32_t> sectionStart;
    };

    enum class LoadResult { NotModern, Modern, Failed };

    /// Demuxes and reshapes an MD21 container in shared->m_data. NotModern: bytes untouched.
    LoadResult PrepareLoad(CM2Shared* shared);

    /// After the stock walk succeeded on a modern model: reference clamps on the walked header.
    void FinishWalk(CM2Shared* shared);

    /// The path to create the texture from, for a texture record the file left unnamed. Returns false when
    /// the model has no TXID answer for it (the caller keeps the stock solid-white placeholder).
    bool TextureName(CM2Shared* shared, uint32_t index, const M2Texture& texture, char* out, size_t cap);

    /// Skin finalize: bone-budget split, contract rebuild for modern models, boneInfluences / shadow gate
    /// fixes. `skinBytes` is the byte size of the skin block as read. Runs before the batch blocks are sized.
    void FinalizeSkin(CM2Shared* shared, uint32_t skinBytes);

    /// A modern external animation file wraps the flat track blob in one chunk; slides the payload onto the
    /// buffer base. Returns true when it unwrapped (size is shortened to the payload).
    bool UnwrapAnim(uint8_t* buffer, uint32_t& size);

    /// Releases everything the model's state owns.
    void Destroy(CM2Shared* shared);

    /// Instances of one batched doodad draw the bone palette can hold: 75 / bones per instance, at least 1.
    inline uint32_t DoodadInstanceCap(uint32_t boneCount)
    {
        return boneCount ? (kMaxBonesPerDraw / boneCount ? kMaxBonesPerDraw / boneCount : 1u) : 0xFFFFFFFFu;
    }

    /// The index start to draw a skin section of the shared's section copy from. Models using the extended
    /// encoding keep it wider than the section record's 16 bits.
    inline uint32_t SectionIndexStart(const CM2Shared* shared, const M2SkinSection* section)
    {
        const State* state = shared ? shared->m_wxl : nullptr;
        if (state && !state->sectionStart.empty() && shared->m_skinSections && section >= shared->m_skinSections)
        {
            const size_t index = static_cast<size_t>(section - shared->m_skinSections);
            if (index < state->sectionStart.size())
                return state->sectionStart[index];
        }
        return section->m_indexStart;
    }

    /// The index start of a section of the skin's own (file-order) section array: the same, for a model whose
    /// sections carry the extended encoding, taken from the full starts kept beside them.
    inline uint32_t SkinIndexStart(const CM2Shared* shared, uint32_t sectionIndex, const M2SkinSection* section)
    {
        const State* state = shared ? shared->m_wxl : nullptr;
        if (state && sectionIndex < state->sourceStart.size())
            return state->sourceStart[sectionIndex];
        return section->m_indexStart;
    }
}
