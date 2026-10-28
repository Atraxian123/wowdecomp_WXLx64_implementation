// Modern M2 reader, skin half: bone-budget split, material / texture-unit contract rebuild, wide index starts.
// Ported from the wxl-modern-m2 extension (Copyright (C) 2026 WarcraftXL, GPLv3, see Engine/Source/WXL/Event.h).
//
// Runs from CM2Shared::FinishLoadingSkinProfile, after the skin's own arrays were walked and before
// InitializeSkinProfile sizes its parallel batch blocks from the skin.
//
// Where the arrays go. An M2Array reaches its data by a signed 32-bit offset from ITSELF, so a rebuilt array
// has to live near its owner. Skin arrays are rebuilt by re-seating the skin block (old contents, then the new
// arrays) and model-header arrays are placed in the spare room the load seated after the model body, falling
// back to an allocation near the model. No pointer is ever stored where an offset belongs.

#include "Engine/Source/Model2/ModernM2/ModernM2.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "Engine/Source/Base/SIMD.h"
#include "Engine/Source/Os/OsMemory.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/WXL/Log.h"

namespace wxl::modernm2
{
    namespace
    {
        constexpr uint32_t kSkinU16Max         = 0xFFFF;  // u16 ceiling of the skin arrays
        constexpr uint32_t kSplitMaxBoneCombos = 0x10000; // boneCombos upper bound, rejected before the bulk copy
        constexpr uint32_t kMaxBatches         = 0x4000;  // a batch / submesh count past this is malformed
        // A texunit shaderId >= this is a source shader-effect index (named modern effect), not a packed
        // blend-bit pair.
        constexpr uint16_t kSourceShaderMin = 0x8000;

        static_assert(sizeof(M2Batch) == 0x18, "batch record");
        static_assert(sizeof(M2SkinSection) == 0x30, "skin section record");

        // The decomp keeps a section's id and its level together in one uint32: id in the low 16 bits.
        inline uint16_t Level(const M2SkinSection& s) { return static_cast<uint16_t>(s.m_skinSectionId >> 16); }
        inline void SetLevel(M2SkinSection& s, uint16_t level)
        {
            s.m_skinSectionId = (s.m_skinSectionId & 0xFFFFu) | (static_cast<uint32_t>(level) << 16);
        }

        bool StartsWithCI(const char* s, const char* prefix)
        {
            for (; *prefix; ++s, ++prefix)
            {
                char a = *s, b = *prefix;
                if (a == '/') a = '\\';
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (a != b) return false;
            }
            return true;
        }

        /// Character and item-component skins can carry more than 65535 indices, so their section level is
        /// repurposed as the high 16 bits of a 32-bit index start; every other source keeps level as the
        /// legacy LOD / sub-batch marker.
        bool UsesExtendedIndexStart(const char* name)
        {
            return name && (StartsWithCI(name, "character\\") || StartsWithCI(name, "item\\objectcomponents\\"));
        }

        inline uint32_t FullIndexStart(const M2SkinSection& s, bool extended)
        {
            return extended ? ((static_cast<uint32_t>(Level(s)) << 16) | s.m_indexStart) : s.m_indexStart;
        }

        struct SplitRun { uint16_t first; uint16_t count; };
        struct SplitSection { M2SkinSection section; uint16_t origSubmesh; };

        /// Working copy of everything the finalize may rewrite, so a failed step leaves the live data alone.
        struct Work
        {
            std::vector<uint16_t>      vtx;        // skin vertex lookup
            std::vector<uint8_t>       bones;      // 4 bone slots per skin vertex
            std::vector<uint16_t>      idx;
            std::vector<M2SkinSection> sections;
            std::vector<M2Batch>       batches;
            bool geometryChanged = false;          // vtx / bones / idx / section count changed

            std::vector<uint16_t> boneCombos;      // model arrays
            uint32_t boneComboCount = 0;
            bool boneCombosChanged = false;
            std::vector<uint16_t> texCoord;        // header's textureCoordCombos
            bool texCoordChanged = false;
            std::vector<uint16_t> combiner;        // header's textureCombinerCombos
            bool combinerChanged = false;
            std::vector<uint16_t> transformCombos; // header's textureTransformCombos
            bool transformChanged = false;
        };

        // ------------------------------------------------------------------------------------ bone split
        bool NeedsSplit(const M2SkinProfile* skin, bool extended)
        {
            const M2SkinSection* sections = skin->m_skinSections.Data();
            for (uint32_t i = 0; i < skin->m_skinSections.m_count; ++i)
                if ((extended || Level(sections[i]) == 0) && sections[i].m_boneCount > kMaxBonesPerDraw) return true;
            return false;
        }

        /// Partitions every drawn submesh whose per-draw bone palette exceeds the client ceiling into
        /// sub-sections within it: a greedy triangle bin-packer, each sub-section with its own compact
        /// boneCombos slice, deduplicated vertex block (bones remapped to the slice) and global-indexed
        /// triangle block. Rebuilds the working skin arrays and boneCombos; false (nothing changed) on any
        /// overflow or missing array, leaving the caller on the clamp path.
        bool SplitSubmeshes(const M2Data* md, Work& w, bool extended, const char* name,
                            std::vector<SplitRun>& splitMap, uint32_t& splitCount)
        {
            const uint32_t boneComboCount = md->m_boneCombos.m_count;
            if (!boneComboCount) return false;
            if (w.vtx.empty() || w.idx.empty() || w.bones.empty()) return false;
            if (boneComboCount > kSplitMaxBoneCombos)
            {
                wxl::LogCat("M2", "'%s' boneCombos count=%u out of range, skipping bone split", name, boneComboCount);
                return false;
            }
            const uint16_t* boneCombos = md->m_boneCombos.Data();
            const uint32_t vertexCount = static_cast<uint32_t>(w.vtx.size());
            const uint32_t indexCount = static_cast<uint32_t>(w.idx.size());

            bool needsSplit = false;
            for (const M2SkinSection& s : w.sections)
                if ((extended || Level(s) == 0) && s.m_boneCount > kMaxBonesPerDraw) { needsSplit = true; break; }
            if (!needsSplit) return false;

            std::vector<uint16_t> newVtx, newIdx, newBoneCombos(boneCombos, boneCombos + boneComboCount);
            std::vector<uint8_t> newBones;
            std::vector<SplitSection> out;
            newVtx.reserve(vertexCount);
            newBones.reserve(vertexCount * 4);
            newIdx.reserve(indexCount);

            splitCount = 0;

            for (uint32_t si = 0; si < w.sections.size(); ++si)
            {
                M2SkinSection s = w.sections[si];

                // A level>0 submesh is a sub-batch the engine cannot draw. Pass it through as one zeroed
                // placeholder so the batch re-point stays 1:1; its batch is later skipped.
                if (Level(s) > 0 && !extended)
                {
                    SetLevel(s, 0);
                    s.m_vertexStart = 0; s.m_vertexCount = 0; s.m_indexStart = 0; s.m_indexCount = 0;
                    s.m_boneComboIndex = 0; s.m_centerBoneIndex = 0; s.m_boneCount = 1;
                    reinterpret_cast<uint8_t*>(&s)[0x11] = 0;
                    out.push_back({ s, static_cast<uint16_t>(si) });
                    continue;
                }

                const uint32_t sourceStart = FullIndexStart(s, extended);
                if (sourceStart > indexCount || s.m_indexCount > indexCount - sourceStart)
                {
                    wxl::LogCat("M2", "'%s' submesh %u index window past skin indexCount, skipping bone split", name, si);
                    return false;
                }

                const uint32_t triCount = s.m_indexCount / 3;
                const uint32_t comboBase = s.m_boneComboIndex;

                std::vector<uint16_t> curGlobals;
                uint32_t curTriStart = 0;
                uint32_t emitted = 0;

                // Emit triangles [triFrom, triTo) as one sub-section.
                auto emit = [&](uint32_t triFrom, uint32_t triTo, std::vector<uint16_t>& globals) -> bool
                {
                    if (triFrom >= triTo) return true;
                    std::sort(globals.begin(), globals.end());
                    const uint32_t comboIndex = static_cast<uint32_t>(newBoneCombos.size());
                    if (comboIndex > kSkinU16Max) return false;
                    for (uint16_t g : globals) newBoneCombos.push_back(g);

                    const uint32_t secVertStart = static_cast<uint32_t>(newVtx.size());
                    const uint32_t secIndexStart = static_cast<uint32_t>(newIdx.size());
                    if (secVertStart > kSkinU16Max) return false;
                    if (!extended && secIndexStart > kSkinU16Max) return false;
                    if (extended && (secIndexStart >> 16) > kSkinU16Max) return false;

                    std::unordered_map<uint16_t, uint16_t> vmap;
                    for (uint32_t t = triFrom; t < triTo; ++t)
                    {
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const uint16_t lv = w.idx[sourceStart + t * 3 + k];
                            if (lv >= vertexCount) return false;
                            auto it = vmap.find(lv);
                            uint16_t nv;
                            if (it == vmap.end())
                            {
                                const uint32_t at = static_cast<uint32_t>(newVtx.size());
                                if (at > kSkinU16Max) return false;
                                nv = static_cast<uint16_t>(at);
                                vmap.emplace(lv, nv);
                                newVtx.push_back(w.vtx[lv]);
                                const uint8_t* infl = w.bones.data() + static_cast<size_t>(lv) * 4;
                                for (uint32_t j = 0; j < 4; ++j)
                                {
                                    const uint32_t comboIdx = comboBase + infl[j];
                                    const uint16_t g = comboIdx < boneComboCount ? boneCombos[comboIdx] : globals[0];
                                    auto lo = std::lower_bound(globals.begin(), globals.end(), g);
                                    const uint16_t local = (lo != globals.end() && *lo == g)
                                                         ? static_cast<uint16_t>(lo - globals.begin()) : 0;
                                    newBones.push_back(static_cast<uint8_t>(local));
                                }
                            }
                            else nv = it->second;
                            newIdx.push_back(nv);
                        }
                    }

                    const uint32_t secVertCount = static_cast<uint32_t>(newVtx.size()) - secVertStart;
                    const uint32_t secIndexCount = static_cast<uint32_t>(newIdx.size()) - secIndexStart;
                    if (secVertCount > kSkinU16Max || secIndexCount > kSkinU16Max) return false;

                    M2SkinSection sec = s;
                    sec.m_vertexStart    = static_cast<uint16_t>(secVertStart);
                    sec.m_vertexCount    = static_cast<uint16_t>(secVertCount);
                    SetLevel(sec, extended ? static_cast<uint16_t>(secIndexStart >> 16) : 0);
                    sec.m_indexStart     = static_cast<uint16_t>(secIndexStart & kSkinU16Max);
                    sec.m_indexCount     = static_cast<uint16_t>(secIndexCount);
                    sec.m_boneCount      = static_cast<uint16_t>(globals.size());
                    sec.m_boneComboIndex = static_cast<uint16_t>(comboIndex);
                    out.push_back({ sec, static_cast<uint16_t>(si) });
                    ++emitted;
                    return true;
                };

                for (uint32_t t = 0; t < triCount; ++t)
                {
                    uint16_t g[12]; int gn = 0;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const uint16_t lv = w.idx[sourceStart + t * 3 + k];
                        if (lv >= vertexCount) return false;
                        const uint8_t* infl = w.bones.data() + static_cast<size_t>(lv) * 4;
                        for (uint32_t j = 0; j < 4; ++j)
                        {
                            const uint32_t comboIdx = comboBase + infl[j];
                            const uint16_t gg = comboIdx < boneComboCount ? boneCombos[comboIdx] : 0;
                            bool seen = false;
                            for (int e = 0; e < gn; ++e) if (g[e] == gg) { seen = true; break; }
                            if (!seen && gn < 12) g[gn++] = gg;
                        }
                    }
                    size_t unionSize = curGlobals.size();
                    for (int e = 0; e < gn; ++e)
                        if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end()) ++unionSize;

                    if (unionSize > kMaxBonesPerDraw && t > curTriStart)
                    {
                        if (!emit(curTriStart, t, curGlobals)) return false;
                        curGlobals.clear();
                        curTriStart = t;
                    }
                    for (int e = 0; e < gn; ++e)
                        if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end())
                            curGlobals.push_back(g[e]);
                }
                if (!emit(curTriStart, triCount, curGlobals)) return false;

                if (emitted == 0)
                {
                    M2SkinSection sec = s;
                    sec.m_vertexCount = 0; sec.m_indexCount = 0; sec.m_boneCount = 1;
                    out.push_back({ sec, static_cast<uint16_t>(si) });
                }
                else if (emitted > 1)
                {
                    splitCount += emitted - 1;
                }
            }

            if (newVtx.size() > kSkinU16Max || out.size() > kMaxBatches) return false;

            splitMap.assign(w.sections.size(), SplitRun{ 0, 0 });
            for (uint16_t i = 0; i < out.size(); ++i)
            {
                const uint16_t orig = out[i].origSubmesh;
                if (orig >= splitMap.size()) continue;
                if (splitMap[orig].count == 0) splitMap[orig].first = i;
                ++splitMap[orig].count;
            }

            w.vtx = std::move(newVtx);
            w.bones = std::move(newBones);
            w.idx = std::move(newIdx);
            w.sections.clear();
            for (const SplitSection& s : out) w.sections.push_back(s.section);
            w.boneCombos = std::move(newBoneCombos);
            w.boneComboCount = static_cast<uint32_t>(w.boneCombos.size());
            w.boneCombosChanged = true;
            w.geometryChanged = true;
            return true;
        }

        /// Re-points a skin's existing batches across the sub-section run their original submesh became,
        /// duplicating a batch per extra sub-section without touching any other field.
        void RepointBatchesAfterSplit(Work& w, const std::vector<SplitRun>& splitMap)
        {
            if (splitMap.empty() || w.batches.empty()) return;
            std::vector<M2Batch> out;
            out.reserve(w.batches.size());
            for (const M2Batch& b : w.batches)
            {
                SplitRun run{ b.m_skinSectionIndex, 1 };
                if (b.m_skinSectionIndex < splitMap.size()) run = splitMap[b.m_skinSectionIndex];
                for (uint16_t s = 0; s < run.count; ++s)
                {
                    M2Batch nb = b;
                    nb.m_skinSectionIndex = static_cast<uint16_t>(run.first + s);
                    out.push_back(nb);
                }
            }
            if (out.empty() || out.size() > kMaxBatches) return;
            w.batches = std::move(out);
        }

        // ------------------------------------------------------------------------------ contract rebuild
        // A source skin encodes each batch's material in its own shaderId (packed blend-bit pair, or a modern
        // effect index) and leaves the header's texture-unit lookup empty. The client's first shader-id pass
        // indexes textureCoordCombos[batch.textureCoordComboIndex] and binds the second texture only when the
        // contract is present, so it is decoded into the fixed-function blend bits the client understands
        // and the lookups are synthesized.

        uint16_t LookupSingle(std::vector<uint16_t>& lookup, int16_t v)
        {
            for (size_t n = 0; n < lookup.size(); ++n)
                if (static_cast<int16_t>(lookup[n]) == v) return static_cast<uint16_t>(n);
            lookup.push_back(static_cast<uint16_t>(v));
            return static_cast<uint16_t>(lookup.size() - 1);
        }

        /// Appends an adjacent pair, reusing an overlapping tail so [base] and [base+1] equal a and b.
        uint16_t LookupPair(std::vector<uint16_t>& lookup, int16_t a, int16_t b)
        {
            for (size_t n = 0; n + 1 < lookup.size(); ++n)
                if (static_cast<int16_t>(lookup[n]) == a && static_cast<int16_t>(lookup[n + 1]) == b)
                    return static_cast<uint16_t>(n);
            const size_t sz = lookup.size();
            if (sz > 1 && static_cast<int16_t>(lookup[sz - 1]) == a)
            {
                lookup.push_back(static_cast<uint16_t>(b));
                return static_cast<uint16_t>(sz - 1);
            }
            lookup.push_back(static_cast<uint16_t>(a));
            lookup.push_back(static_cast<uint16_t>(b));
            return static_cast<uint16_t>(sz);
        }

        /// Finds or appends a (blend1, blend2) pair in textureCombinerCombos.
        uint16_t BlendOverride(std::vector<uint16_t>& combos, uint16_t b1, uint16_t b2)
        {
            for (size_t n = 0; n + 1 < combos.size(); n += 2)
                if (combos[n] == b1 && combos[n + 1] == b2) return static_cast<uint16_t>(n);
            const uint16_t base = static_cast<uint16_t>(combos.size());
            combos.push_back(b1);
            combos.push_back(b2);
            return base;
        }

        uint16_t NextLookup(uint16_t base, uint32_t count)
        {
            const uint32_t next = static_cast<uint32_t>(base) + 1u;
            return next < count ? static_cast<uint16_t>(next) : base;
        }

        /// Parks LOD submeshes and clamps drawn-submesh bone counts to the client ceiling. A level>0 (LOD)
        /// submesh has its geometry zeroed with boneCount kept >= 1 (the native finalize divides the budget
        /// by every submesh boneCount); a drawn submesh's boneCount is clamped to the ceiling and to the
        /// boneCombos bounds. A zero-geometry section is marked bad.
        void FixSubmeshes(Work& w, bool extended, std::vector<uint8_t>& bad, const char* name)
        {
            const uint32_t indexCount = static_cast<uint32_t>(w.idx.size());
            bad.assign(w.sections.size(), 0);
            for (uint32_t i = 0; i < w.sections.size(); ++i)
            {
                M2SkinSection& s = w.sections[i];
                if (Level(s) > 0 && !extended)
                {
                    SetLevel(s, 0);
                    s.m_vertexStart = 0; s.m_vertexCount = 0; s.m_indexStart = 0; s.m_indexCount = 0;
                    s.m_boneComboIndex = 0; s.m_centerBoneIndex = 0;
                }
                else if (s.m_indexCount != 0)
                {
                    const uint32_t start = FullIndexStart(s, extended);
                    if (start > indexCount || s.m_indexCount > indexCount - start)
                    {
                        wxl::LogCat("M2", "'%s' submesh %u index window past skin indexCount, parking", name, i);
                        SetLevel(s, 0);
                        s.m_vertexStart = 0; s.m_vertexCount = 0; s.m_indexStart = 0; s.m_indexCount = 0;
                        s.m_boneComboIndex = 0; s.m_centerBoneIndex = 0;
                    }
                }

                if (s.m_indexCount == 0)
                {
                    if (s.m_boneCount < 1) s.m_boneCount = 1;
                    bad[i] = 1;
                }
                else
                {
                    uint16_t cap = static_cast<uint16_t>(kMaxBonesPerDraw);
                    const uint16_t byCombo = w.boneComboCount > s.m_boneComboIndex
                                           ? static_cast<uint16_t>(w.boneComboCount - s.m_boneComboIndex) : 1;
                    if (byCombo < cap) cap = byCombo;
                    if (cap < 1) cap = 1;
                    if (s.m_boneCount > cap) s.m_boneCount = cap;
                    if (s.m_boneCount < 1) s.m_boneCount = 1;
                    if (s.m_boneInfluences == 0) s.m_boneInfluences = 1;
                }
                reinterpret_cast<uint8_t*>(&s)[0x11] = 0;
            }
        }

        /// Decodes a < kSourceShaderMin shaderId's blend bits and synthesizes its lookup entries, in place.
        void DecodeBlendBits(M2Batch& b, uint16_t shaderId, uint16_t textureCount,
                             std::vector<uint16_t>& texCoord, std::vector<uint16_t>& blendOverride)
        {
            const uint16_t blend1 = (shaderId >> 4) & 0x7;
            const uint16_t blend2 = shaderId & 0x7;

            const bool twoTex = textureCount > 1 && (shaderId & 0x4000) && blend1 != 0 && blend2 != 0;
            uint16_t shaderToSave = 0;
            if (twoTex) shaderToSave = BlendOverride(blendOverride, blend1, blend2);
            else        textureCount = 1;

            b.m_flags &= 0x10;
            b.m_shader = shaderToSave;

            if (textureCount == 1)
            {
                const int16_t t0 = (shaderId & 0x80) ? -1 : 0;
                b.m_textureCoordComboIndex = LookupSingle(texCoord, t0);
            }
            else
            {
                int16_t t0, t1;
                if (shaderId & 0x80) { t0 = -1; t1 = (shaderId & 0x8) ? -1 : 0; }
                else { t0 = 0; t1 = (shaderId & 0x8) ? -1 : ((shaderId & 0x4000) ? 1 : 0); }
                b.m_textureCoordComboIndex = LookupPair(texCoord, t0, t1);
            }

            b.m_textureCount = textureCount < 2 ? textureCount : 2;
        }

        /// Splits one source env batch into a primary plus a follower second render pass over the same
        /// geometry: the follower carries material layer 1 and material index +1 so the engine binds it as
        /// the layered second pass over the diffuse below it. Returns false when `low` is not an env split.
        bool EnvSplit(uint16_t low, uint16_t shaderId, M2Batch primary, uint32_t nTransparencyLookup,
                      uint32_t nTransformLookup, std::vector<M2Batch>& out,
                      std::vector<uint16_t>& texCoord, std::vector<uint16_t>& blendOverride)
        {
            M2Batch follower = primary;
            follower.m_materialIndex = static_cast<uint16_t>(primary.m_materialIndex + 1);
            follower.m_materialLayer = 1;
            follower.m_textureCount = 1;

            switch (low)
            {
            case 0: case 3: case 9: case 17: case 24:
            {
                primary.m_textureCount = 2;
                const uint16_t blendIdx = BlendOverride(blendOverride, 1, 4);
                const uint16_t tc = LookupPair(texCoord, 0, -1);
                primary.m_shader = blendIdx;  primary.m_textureCoordComboIndex = tc;
                follower.m_shader = blendIdx; follower.m_textureCoordComboIndex = tc;
                out.push_back(primary); out.push_back(follower);
                return true;
            }
            case 1: case 15:
            {
                primary.m_textureCount = 1;
                primary.m_shader = 0; follower.m_shader = 0;
                follower.m_textureComboIndex = static_cast<uint16_t>(primary.m_textureComboIndex + 1);
                if (static_cast<uint32_t>(primary.m_textureWeightComboIndex) + 1 < nTransparencyLookup)
                    follower.m_textureWeightComboIndex = static_cast<uint16_t>(primary.m_textureWeightComboIndex + 1);
                follower.m_textureTransformComboIndex = NextLookup(primary.m_textureTransformComboIndex, nTransformLookup);
                const int16_t t1 = (shaderId == 0x8001) ? -1 : 1;
                const uint16_t tc = LookupPair(texCoord, 0, t1);
                primary.m_textureCoordComboIndex = tc;
                follower.m_textureCoordComboIndex = static_cast<uint16_t>(tc + 1);
                out.push_back(primary); out.push_back(follower);
                return true;
            }
            case 2:
            {
                const uint16_t blendIdx = BlendOverride(blendOverride, 1, 3);
                const uint16_t tc = LookupPair(texCoord, 0, -1);
                primary.m_shader = blendIdx;  primary.m_textureCoordComboIndex = tc;
                follower.m_shader = blendIdx; follower.m_textureCoordComboIndex = tc;
                out.push_back(primary); out.push_back(follower);
                return true;
            }
            default:
                return false;
            }
        }

        /// Reshapes one batch into 'piece' (one batch, or primary plus follower for an env split). Does not
        /// touch skinSectionIndex; the caller re-points it per target sub-section.
        void DownConvertBatch(M2Batch b, uint32_t nTransparencyLookup, uint32_t nTransformLookup,
                              std::vector<M2Batch>& piece, std::vector<uint16_t>& texCoord,
                              std::vector<uint16_t>& blendOverride)
        {
            uint16_t shaderId = b.m_shader;
            uint16_t textureCount = b.m_textureCount;
            b.m_flags &= 0x10;

            if (shaderId >= kSourceShaderMin)
            {
                // Source shader-effect indices [0..2] are Diffuse_T1_Env effects the engine renders natively,
                // re-based to engine index = sourceIdx + 1 (index 0 is "no shader"). One 2-texture batch
                // (T1 + env coord); not routed through the env-split heuristic.
                const uint16_t sourceIdx = shaderId & 0x7fff;
                if (sourceIdx <= 2)
                {
                    b.m_shader = static_cast<uint16_t>(kSourceShaderMin | (sourceIdx + 1));
                    b.m_textureCount = 2;
                    b.m_textureCoordComboIndex = LookupPair(texCoord, 0, -1);
                    b.m_flags &= 0x10;
                    piece.push_back(b);
                    return;
                }

                const uint16_t low = shaderId & 0xFF;
                if (EnvSplit(low, shaderId, b, nTransparencyLookup, nTransformLookup, piece, texCoord, blendOverride))
                    return;
                switch (low)
                {
                case 5: case 8: case 10: case 12: case 16: case 23:
                    shaderId = 0; textureCount = 1; break;
                case 21: case 33: case 35:
                    // The additive-overlay combiner. The extension substituted its pixel shader at draw time
                    // (CombinerPatch, D3D9 bytecode); here it keeps the two-texture blend decode only.
                    shaderId = 0x4011; textureCount = 2; break;
                default:
                    shaderId = 0x0010; textureCount = 1; break;
                }
            }

            if (shaderId < kSourceShaderMin) DecodeBlendBits(b, shaderId, textureCount, texCoord, blendOverride);
            else b.m_textureCount = textureCount < 2 ? textureCount : 2;

            piece.push_back(b);
        }

        /// Builds the reshaped batch array: each original batch is processed once, then emitted for every
        /// sub-section its original submesh became (skinSectionIndex re-pointed). A batch on a bad section
        /// is reduced to a no-draw batch.
        void FixTexUnits(Work& w, const std::vector<uint8_t>& bad, const std::vector<SplitRun>& splitMap,
                         std::vector<M2Batch>& out, uint32_t nTransparencyLookup, uint32_t nTransformLookup)
        {
            out.reserve(w.batches.size());
            for (const M2Batch& original : w.batches)
            {
                SplitRun run{ original.m_skinSectionIndex, 1 };
                if (original.m_skinSectionIndex < splitMap.size()) run = splitMap[original.m_skinSectionIndex];

                std::vector<M2Batch> piece;
                DownConvertBatch(original, nTransparencyLookup, nTransformLookup, piece, w.texCoord, w.combiner);

                for (uint16_t s = 0; s < run.count; ++s)
                {
                    const uint16_t sectionIdx = static_cast<uint16_t>(run.first + s);
                    const bool isBad = sectionIdx < bad.size() && bad[sectionIdx];
                    for (const M2Batch& p : piece)
                    {
                        M2Batch nb = p;
                        nb.m_skinSectionIndex = sectionIdx;
                        if (isBad) nb.m_shader = 0x8000;
                        out.push_back(nb);
                    }
                }
            }
        }

        /// A model with texture transforms but no transform lookup gets the identity lookup, extended to
        /// cover every index its batches name.
        void EnsureTextureTransformLookup(const M2Data* md, Work& w, const char* name)
        {
            if (!md->m_textureTransforms.m_count || md->m_textureTransformCombos.m_count) return;

            uint32_t needed = md->m_textureTransforms.m_count;
            for (const M2Batch& b : w.batches)
            {
                const uint32_t base = b.m_textureTransformComboIndex;
                if (base == 0xFFFFu) continue;
                const uint32_t count = b.m_textureCount ? b.m_textureCount : 1u;
                if (base <= 0xFFFFu - count && needed < base + count) needed = base + count;
            }

            w.transformCombos.resize(needed);
            for (uint32_t i = 0; i < needed; ++i)
                w.transformCombos[i] = i < md->m_textureTransforms.m_count ? static_cast<uint16_t>(i) : static_cast<uint16_t>(0xFFFF);
            w.transformChanged = true;
            wxl::LogCat("M2", "'%s' synthesized %u texture-transform lookup entries", name, needed);
        }

        /// Render-flag bits this client understands and that must survive the mask:
        ///   0x00-0x1F the classic render flags; 0x40 SHADOW-EXCLUDE (the stock caster collector drops the
        ///   batch outright); 0x80 SHADOW-FORCE (casts even when the blend mode would not qualify).
        constexpr uint16_t kMaterialFlagMask = 0x1Fu | 0x40u | 0x80u;

        void FixRenderFlags(M2Data* md)
        {
            for (uint32_t i = 0; i < md->m_materials.m_count; ++i)
            {
                M2Material& m = md->m_materials[i];
                if (m.m_blendMode > 6) { m.m_blendMode = 4; m.m_flags |= 0x5; }
                m.m_flags &= kMaterialFlagMask;
            }
        }

        /// Rebuilds the material / texunit contract on a modern model's working skin.
        void Rebuild(M2Data* md, Work& w, bool extended, const std::vector<SplitRun>& splitMap, const char* name)
        {
            if (w.batches.size() > kMaxBatches)
            {
                wxl::LogCat("M2", "'%s' skin batchCount=%u exceeds cap, clamping", name, static_cast<uint32_t>(w.batches.size()));
                w.batches.resize(kMaxBatches);
            }

            const uint32_t nTransparencyLookup = md->m_textureWeightCombos.m_count;
            EnsureTextureTransformLookup(md, w, name);
            const uint32_t nTransformLookup = w.transformChanged ? static_cast<uint32_t>(w.transformCombos.size())
                                                                 : md->m_textureTransformCombos.m_count;

            std::vector<uint8_t> bad;
            std::vector<M2Batch> batches;
            FixSubmeshes(w, extended, bad, name);
            FixTexUnits(w, bad, splitMap, batches, nTransparencyLookup, nTransformLookup);

            if (!batches.empty()) w.batches = std::move(batches);
            if (!w.texCoord.empty())  w.texCoordChanged = true;
            if (!w.combiner.empty())  w.combinerChanged = true;
            FixRenderFlags(md);
        }

        // -------------------------------------------------------------------------------------- commit
        void* PlaceInModelBlock(CM2Shared* shared, State& state, size_t bytes)
        {
            bytes = (bytes + 15) & ~static_cast<size_t>(15);
            if (state.reserveUsed + bytes <= state.reserveCap)
            {
                uint8_t* p = reinterpret_cast<uint8_t*>(shared->m_data) + state.reserveBase + state.reserveUsed;
                state.reserveUsed += static_cast<uint32_t>(bytes);
                return p;
            }
#if defined(WOWDECOMP_ARCH_64)
            void* p = OsAllocNear(shared->m_data, bytes);
#else
            void* p = SMemAlloc(static_cast<uint32_t>(bytes), __FILE__, __LINE__, 0);
#endif
            if (p) state.farBlocks.push_back(p);
            return p;
        }

        State& EnsureState(CM2Shared* shared)
        {
            if (!shared->m_wxl) shared->m_wxl = new State();
            return *shared->m_wxl;
        }

        template <class T>
        bool SetArray(M2Array<T>& array, T* data, uint32_t count)
        {
            array.m_count = count;
            array.Set(count ? data : nullptr);
            return !count || array.Data() == data; // false when the data is out of reach of a 32-bit offset
        }

        /// Writes the working copy back: model arrays into the model block (or near it), skin arrays into a
        /// re-seated skin block when their size changed. Memory is gathered first and nothing is modified
        /// until every allocation succeeded.
        bool Commit(CM2Shared* shared, uint32_t skinBytes, Work& w, const char* name)
        {
            M2Data* md = shared->m_data;
            M2SkinProfile* sk = shared->m_skinData;
            State& state = EnsureState(shared);

            // --- model-level arrays
            uint16_t* pBoneCombos = nullptr; uint16_t* pTexCoord = nullptr;
            uint16_t* pCombiner = nullptr;   uint16_t* pTransform = nullptr;
            if (w.boneCombosChanged && !(pBoneCombos = static_cast<uint16_t*>(PlaceInModelBlock(shared, state, w.boneCombos.size() * 2)))) return false;
            if (w.texCoordChanged && !(pTexCoord = static_cast<uint16_t*>(PlaceInModelBlock(shared, state, w.texCoord.size() * 2)))) return false;
            if (w.combinerChanged && !(pCombiner = static_cast<uint16_t*>(PlaceInModelBlock(shared, state, w.combiner.size() * 2)))) return false;
            if (w.transformChanged && !(pTransform = static_cast<uint16_t*>(PlaceInModelBlock(shared, state, w.transformCombos.size() * 2)))) return false;

            // --- skin block
            const bool reseat = w.geometryChanged || w.batches.size() != sk->m_batches.m_count;
            uint8_t* newBlock = nullptr;
            size_t offVtx = 0, offBones = 0, offIdx = 0, offSections = 0, offBatches = 0;
            if (reseat)
            {
                size_t at = (static_cast<size_t>(skinBytes) + 15) & ~static_cast<size_t>(15);
                auto take = [&](size_t bytes) { const size_t o = at; at += (bytes + 15) & ~static_cast<size_t>(15); return o; };
                if (w.geometryChanged)
                {
                    offVtx = take(w.vtx.size() * 2);
                    offBones = take(w.bones.size());
                    offIdx = take(w.idx.size() * 2);
                }
                offSections = take(w.sections.size() * sizeof(M2SkinSection));
                offBatches = take(w.batches.size() * sizeof(M2Batch));
                newBlock = static_cast<uint8_t*>(SIMD::AllocA16(static_cast<uint32_t>(at), __FILE__, __LINE__));
                if (!newBlock) return false;
            }

            // --- apply: model arrays
            bool ok = true;
            if (w.boneCombosChanged)
            {
                std::memcpy(pBoneCombos, w.boneCombos.data(), w.boneCombos.size() * 2);
                ok &= SetArray(md->m_boneCombos, pBoneCombos, static_cast<uint32_t>(w.boneCombos.size()));
            }
            if (w.texCoordChanged)
            {
                std::memcpy(pTexCoord, w.texCoord.data(), w.texCoord.size() * 2);
                ok &= SetArray(md->m_textureCoordCombos, pTexCoord, static_cast<uint32_t>(w.texCoord.size()));
            }
            if (w.combinerChanged)
            {
                std::memcpy(pCombiner, w.combiner.data(), w.combiner.size() * 2);
                ok &= SetArray(md->m_textureCombinerCombos, pCombiner, static_cast<uint32_t>(w.combiner.size()));
                md->m_flags |= 0x8; // use the texture combiner combos
            }
            if (w.transformChanged)
            {
                std::memcpy(pTransform, w.transformCombos.data(), w.transformCombos.size() * 2);
                ok &= SetArray(md->m_textureTransformCombos, pTransform, static_cast<uint32_t>(w.transformCombos.size()));
            }

            // --- apply: skin arrays
            if (reseat)
            {
                std::memcpy(newBlock, sk, skinBytes);
                auto* nsk = reinterpret_cast<M2SkinProfile*>(newBlock);
                if (w.geometryChanged)
                {
                    std::memcpy(newBlock + offVtx, w.vtx.data(), w.vtx.size() * 2);
                    std::memcpy(newBlock + offBones, w.bones.data(), w.bones.size());
                    std::memcpy(newBlock + offIdx, w.idx.data(), w.idx.size() * 2);
                    ok &= SetArray(nsk->m_vertices, reinterpret_cast<uint16_t*>(newBlock + offVtx), static_cast<uint32_t>(w.vtx.size()));
                    ok &= SetArray(nsk->m_bones, reinterpret_cast<ubyte4*>(newBlock + offBones), static_cast<uint32_t>(w.bones.size() / 4));
                    ok &= SetArray(nsk->m_indices, reinterpret_cast<uint16_t*>(newBlock + offIdx), static_cast<uint32_t>(w.idx.size()));
                }
                std::memcpy(newBlock + offSections, w.sections.data(), w.sections.size() * sizeof(M2SkinSection));
                std::memcpy(newBlock + offBatches, w.batches.data(), w.batches.size() * sizeof(M2Batch));
                ok &= SetArray(nsk->m_skinSections, reinterpret_cast<M2SkinSection*>(newBlock + offSections), static_cast<uint32_t>(w.sections.size()));
                ok &= SetArray(nsk->m_batches, reinterpret_cast<M2Batch*>(newBlock + offBatches), static_cast<uint32_t>(w.batches.size()));
                shared->m_skinData = nsk;
                SIMD::FreeA16(sk);
            }
            else
            {
                std::memcpy(sk->m_skinSections.Data(), w.sections.data(), w.sections.size() * sizeof(M2SkinSection));
                if (!w.batches.empty())
                    std::memcpy(sk->m_batches.Data(), w.batches.data(), w.batches.size() * sizeof(M2Batch));
            }

            if (!ok)
                wxl::LogCat("M2", "'%s' rebuilt array out of reach of its 32-bit offset", name);
            return ok;
        }

        /// A section with boneInfluences == 0 takes the client's shadow-map vertex variant that skips the
        /// inverse(cameraView) * lightView rebase, which makes a shadow stay rigidly aligned to the camera
        /// instead of the light. Every drawn submesh with boneInfluences == 0 is lifted to 1.
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

    // ----------------------------------------------------------------------------------------- FinalizeSkin
    void FinalizeSkin(CM2Shared* shared, uint32_t skinBytes)
    {
        M2Data* md = shared->m_data;
        M2SkinProfile* sk = shared->m_skinData;
        if (!md || !sk) return;

        const char* name = shared->m_fileName;
        const State* existing = shared->m_wxl;
        const bool modern = existing && existing->modern;
        const bool extended = UsesExtendedIndexStart(name);

        bool anyLevel = false;
        if (extended)
        {
            const M2SkinSection* sections = sk->m_skinSections.Data();
            for (uint32_t i = 0; i < sk->m_skinSections.m_count; ++i)
                if (Level(sections[i]) != 0) { anyLevel = true; break; }
        }

        const bool split = NeedsSplit(sk, extended);

        // Stock content with nothing to rebuild: only the two unconditional fixes.
        if (!modern && !split && !anyLevel)
        {
            NormalizeBoneInfluences(sk);
            ForceShadowAnimGate(shared);
            return;
        }

        Work w;
        {
            const uint32_t vc = sk->m_vertices.m_count;
            const uint16_t* vtx = sk->m_vertices.Data();
            w.vtx.assign(vtx, vtx + vc);
            const ubyte4* bones = sk->m_bones.Data();
            w.bones.resize(static_cast<size_t>(sk->m_bones.m_count) * 4);
            for (uint32_t i = 0; i < sk->m_bones.m_count; ++i) std::memcpy(&w.bones[static_cast<size_t>(i) * 4], bones[i].b, 4);
            const uint16_t* idx = sk->m_indices.Data();
            w.idx.assign(idx, idx + sk->m_indices.m_count);
            const M2SkinSection* sec = sk->m_skinSections.Data();
            w.sections.assign(sec, sec + sk->m_skinSections.m_count);
            const M2Batch* bat = sk->m_batches.Data();
            w.batches.assign(bat, bat + sk->m_batches.m_count);
            w.boneComboCount = md->m_boneCombos.m_count;
        }

        // The bone-budget split is a hard client-engine constraint, not a format concern: it runs for every
        // model whatever its origin.
        std::vector<SplitRun> splitMap;
        uint32_t splitCount = 0;
        const bool didSplit = split && SplitSubmeshes(md, w, extended, name, splitMap, splitCount) && splitCount > 0;
        if (didSplit)
            wxl::LogCat("M2", "'%s' bone splitter produced %u extra sub-draw(s)", name, splitCount);

        if (modern)
        {
            // The shaderId decode + texture-unit synthesis assumes the packed modern encoding the loader
            // leaves on the skin; a stock v264 skin already carries a resolved contract.
            Rebuild(md, w, extended, splitMap, name);
        }
        else
        {
            if (didSplit) RepointBatchesAfterSplit(w, splitMap);
            for (M2SkinSection& s : w.sections)
                if (s.m_indexCount != 0 && s.m_boneInfluences == 0) s.m_boneInfluences = 1;
        }

        // Extended encoding: the high half of an index start rides in the section's level. Take it out of
        // the section id (the rest of the client reads that as a plain id) and keep the full starts here.
        std::vector<uint32_t> sourceStart;
        if (extended && anyLevel)
        {
            sourceStart.resize(w.sections.size());
            for (size_t i = 0; i < w.sections.size(); ++i)
            {
                sourceStart[i] = FullIndexStart(w.sections[i], true);
                SetLevel(w.sections[i], 0);
            }
        }

        if (!Commit(shared, skinBytes, w, name))
            wxl::LogCat("M2", "'%s' skin finalize could not commit", name);

        if (!sourceStart.empty())
        {
            State& state = EnsureState(shared);
            state.extendedIndexStart = true;
            state.sectionStart = sourceStart;
            state.sourceStart = std::move(sourceStart);
        }

        ForceShadowAnimGate(shared);
    }
}
