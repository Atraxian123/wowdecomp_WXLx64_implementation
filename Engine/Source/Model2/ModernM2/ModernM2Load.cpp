// Modern M2 reader, load half: container demux, in-place field deltas, record normalization, TXID names.
// Ported from the wxl-modern-m2 extension (Copyright (C) 2026 WarcraftXL, GPLv3, see Engine/Source/WXL/Event.h).
//
// Everything here runs on the raw body BEFORE the stock M2Init walk, while array offsets are still
// body-relative: a record can still be moved, and a nested array is still addressed by body offset. After
// PrepareLoad the buffer is a flat MD20 body in the shape the client's own walk expects, and the walk is the
// stock one.

#include "Engine/Source/Model2/ModernM2/ModernM2.h"
#include "Engine/Source/Model2/ModernM2/FlightAnim.h"

#include <atomic>
#include <cmath>
#include <cstdio>
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
#include "Engine/Source/WXL/Sdk/wxl_listfile_api.h"

namespace wxl::modernm2
{
    namespace
    {
        // ------------------------------------------------------------------------------ byte helpers
        inline uint32_t Rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        inline uint16_t Rd16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        inline void     Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
        inline void     Wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }

        /// MD21-style chunk tags are read as the little-endian dword of the on-disk bytes ('MD21' reads back
        /// 0x3132444D); they are NOT byte-reversed.
        constexpr uint32_t Tag(const char (&s)[5])
        {
            return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) |
                   (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24);
        }

        constexpr uint32_t kTagMD21 = Tag("MD21");
        constexpr uint32_t kTagMD20 = Tag("MD20");
        constexpr uint32_t kTagTXID = Tag("TXID");
        constexpr uint32_t kTagSFID = Tag("SFID");
        constexpr uint32_t kTagTXAC = Tag("TXAC");
        constexpr uint32_t kTagLDV1 = Tag("LDV1");
        constexpr uint32_t kTagAFID = Tag("AFID");
        constexpr uint32_t kTagSKID = Tag("SKID");
        constexpr uint32_t kTagPFID = Tag("PFID");
        constexpr uint32_t kTagBFID = Tag("BFID");
        constexpr uint32_t kTagAFM2 = Tag("AFM2");

        /// True when count records of stride bytes starting at offset all lie inside a body of size bytes
        /// (64-bit product, so a hostile count cannot wrap past the bound).
        inline bool FitsInBody(uint32_t size, uint32_t offset, uint32_t count, uint32_t stride)
        {
            return offset <= size && static_cast<uint64_t>(count) * stride <= static_cast<uint64_t>(size) - offset;
        }

        // skipMask bits, reported in the load log.
        enum : uint32_t
        {
            kSkipTxac = 0x01, kSkipLdv1 = 0x02, kSkipAfid = 0x04, kSkipSkid = 0x08, kSkipPhysBone = 0x10,
            kSkipOther = 0x80,
        };

        struct Scan
        {
            uint32_t bodyOff;
            uint32_t bodySize;
            uint32_t txid[kMaxTxid];
            uint32_t txidCount;
            uint32_t sfidFirst;
            uint32_t sfidCount;
            uint32_t skipMask;
        };

        /// One chunk walk: harvests the MD20 body location and the auxiliary chunks.
        bool ScanContainer(const uint8_t* buf, uint32_t size, Scan& s)
        {
            std::memset(&s, 0, sizeof s);
            uint32_t at = 0;
            while (at + 8 <= size)
            {
                const uint32_t tag = Rd32(buf + at);
                const uint32_t sz = Rd32(buf + at + 4);
                if (sz > size || at + 8 + sz > size) break; // malformed tail; keep what we have
                const uint8_t* payload = buf + at + 8;

                switch (tag)
                {
                case kTagMD21: s.bodyOff = at + 8; s.bodySize = sz; break;
                case kTagTXID:
                {
                    uint32_t n = sz / 4;
                    if (n > kMaxTxid) n = kMaxTxid;
                    for (uint32_t i = 0; i < n; ++i) s.txid[i] = Rd32(payload + i * 4);
                    s.txidCount = n;
                    break;
                }
                case kTagSFID: s.sfidCount = sz / 4; if (s.sfidCount) s.sfidFirst = Rd32(payload); break;
                case kTagTXAC: s.skipMask |= kSkipTxac; break;
                case kTagLDV1: s.skipMask |= kSkipLdv1; break;
                case kTagAFID: s.skipMask |= kSkipAfid; break;
                case kTagSKID: s.skipMask |= kSkipSkid; break;
                case kTagPFID:
                case kTagBFID: s.skipMask |= kSkipPhysBone; break;
                default:       s.skipMask |= kSkipOther; break;
                }
                at += 8 + sz;
            }
            return s.bodyOff != 0 && s.bodySize >= sizeof(M2Data) && s.bodyOff + s.bodySize <= size;
        }

        // --------------------------------------------------------------------- in-place field deltas
        /// Sequence deltas: masks the split u16 in|out blendTime to the client's single value (read whole it
        /// would be a huge blend, so transitions never complete) and remaps the curated set of source ids that
        /// have a client counterpart onto client ids (the flight set onto its own FLY_SWIM twins, the takeoff /
        /// landing set onto the jump ids), patching the lookup. Every other id keeps its own number: the client's
        /// table (ANIM_COUNT) now covers the modern one. Counts the sequences whose data
        /// streams from a .anim file (flags bit 0x20 clear).
        uint32_t FixSequences(uint8_t* base, uint32_t size, M2Data* h)
        {
            uint32_t external = 0;
            if (!h->m_sequences.m_count || !h->m_sequences.m_offset) return 0;
            if (!FitsInBody(size, h->m_sequences.m_offset, h->m_sequences.m_count, sizeof(M2Sequence))) return 0;
            auto* seqs = reinterpret_cast<M2Sequence*>(base + h->m_sequences.m_offset);

            int16_t* lookup = nullptr;
            uint32_t lookupCount = 0;
            if (h->m_sequenceIdxHashById.m_count && h->m_sequenceIdxHashById.m_offset &&
                FitsInBody(size, h->m_sequenceIdxHashById.m_offset, h->m_sequenceIdxHashById.m_count, 2))
            {
                lookup = reinterpret_cast<int16_t*>(base + h->m_sequenceIdxHashById.m_offset);
                lookupCount = h->m_sequenceIdxHashById.m_count;
            }

            auto indexById = [&](uint16_t anim) -> int16_t
            {
                for (uint32_t i = 0; i < h->m_sequences.m_count; ++i)
                    if (seqs[i].m_id == anim) return static_cast<int16_t>(i);
                return -1;
            };
            auto patchLookup = [&](int16_t oldPos, uint16_t newId, int16_t newPos)
            {
                if (!lookup) return;
                if (newId < lookupCount && lookup[newId] == oldPos) { lookup[newId] = newPos; return; }
                for (uint32_t i = 0; i < lookupCount; ++i)
                    if (lookup[i] == oldPos) { lookup[i] = newPos; break; }
            };

            for (uint32_t i = 0; i < h->m_sequences.m_count; ++i)
            {
                const uint16_t id = seqs[i].m_id;
                uint16_t anim = id;

                // The flight set is filed under its own twin ids (the client's FLY_SWIM family), NOT on top of
                // the swim ids: a model with both keeps both, and the game layer asks for the flight twin
                // first and falls back to swim (see FlightAnim.h).
                if (const uint32_t flight = wxl::flightanim::FlightIdForSource(id))
                {
                    anim = static_cast<uint16_t>(flight);
                }
                else
                {
                    switch (id)
                    {
                    case 564: anim = 37;  break;
                    case 572: anim = 39;  break;
                    case 574: anim = 187; break;
                    }
                }
                if (anim != id)
                {
                    patchLookup(indexById(anim), anim, static_cast<int16_t>(i));
                    seqs[i].m_id = anim;
                }
                seqs[i].m_blendTime &= 0xFFFFu;
                if (!(seqs[i].m_flags & 0x20u)) ++external;
            }
            return external;
        }

        /// Material deltas: a blend mode above the client's 7-entry table is clamped to Add (4, flags |0x5),
        /// and the flags are masked to the bits this client understands (the skin finalize re-applies the
        /// same contract idempotently).
        void FixMaterials(uint8_t* base, uint32_t size, M2Data* h)
        {
            if (!h->m_materials.m_count || !h->m_materials.m_offset) return;
            if (!FitsInBody(size, h->m_materials.m_offset, h->m_materials.m_count, sizeof(M2Material))) return;
            auto* mats = reinterpret_cast<M2Material*>(base + h->m_materials.m_offset);
            for (uint32_t i = 0; i < h->m_materials.m_count; ++i)
            {
                if (mats[i].m_blendMode > 6) { mats[i].m_blendMode = 4; mats[i].m_flags |= 0x5; }
                mats[i].m_flags &= 0x1F;
            }
        }

        // ------------------------------------------------------------------------ record normalization
        // Record widths and record SHAPES are the only things that move between source eras. Every widened
        // record shrinks, which is what makes the rewrite in place and allocation-free: the buffer keeps its
        // identity and its size, only the used span of each array gets shorter, and the header count is
        // unchanged. After the pass the body carries one record shape whatever era the model came from.

        // Client record widths (the shapes the engine steps).
        constexpr uint32_t kParticleStrideClient = 0x1DC;
        constexpr uint32_t kCameraStrideClient   = 0x64;
        static_assert(sizeof(M2Particle) == kParticleStrideClient, "client particle record");
        static_assert(sizeof(M2Camera) == kCameraStrideClient, "client camera record");

        // Source-era record widths. The particle record is the client one with the multi-texture scroll
        // parameters (two C2Vector pairs) appended; the camera record trades its leading fov float for a
        // trailing fov track (0x60 body end + one 0x14 track).
        // VERIFY: these three constants came from the format documentation, not from the extension (its
        // offsets/game/M2.hpp is not part of the package); check them against that header.
        constexpr uint32_t kParticleStrideModern = kParticleStrideClient + 0x20;
        constexpr uint32_t kCameraStrideModern   = 0x60 + 0x14;
        constexpr uint32_t kParticleModernMinVer = 272;
        constexpr uint32_t kCameraModernMinVer   = 272;

        // M2Particle.flags bit: the emitter packs three layer ids into its texture slot.
        constexpr uint32_t kParticleFlagMultiTex = 0x10000000;

        struct NormalizeCtx
        {
            uint8_t* body;
            uint32_t size;
            M2Data*  header;
        };

        // An animation track heads its two count+offset pairs with the interpolation fields; the value pair
        // is the second one, and it is a slot list (one entry per sequence, or the global loop's own) of
        // key lists.
        constexpr uint32_t kTrackGlobalLoop = 0x02;
        constexpr uint32_t kTrackValues     = 0x0C;
        constexpr int16_t  kNoGlobalLoop    = -1;
        // A sequence keeps its keys in this model only when this bit is set; otherwise they arrive with a
        // companion file and the offsets in the record do not address this body at all.
        constexpr uint32_t kSeqKeysInline   = 0x20;

        // Particle: fields of one emitter record.
        constexpr uint32_t kPOffFlags        = 0x004;
        constexpr uint32_t kPOffTextureId    = 0x016;
        constexpr uint32_t kPOffGravityTrack = 0x084;
        static_assert(offsetof(M2Particle, m_flags) == kPOffFlags, "particle flags");
        static_assert(offsetof(M2Particle, m_textureIndex) == kPOffTextureId, "particle texture");
        static_assert(offsetof(M2Particle, m_gravityTrack) == kPOffGravityTrack, "particle gravity track");
        constexpr uint16_t kPackedTextureIdMask = 0x1F;
        constexpr uint32_t kFlagCompressedGravity = 0x800000;
        constexpr float    kGravityMagnitudeUnit  = 0.04238648f;
        constexpr float    kPackedDirectionUnit   = 1.0f / 128.0f;

        /// Expands packed gravity keys ({int8 x, int8 y, int16 magnitude}) into the plain downward scalar
        /// the client reads, and clears the flag that said they were packed.
        bool ExpandGravityKeys(const NormalizeCtx& ctx, uint8_t* rec, uint32_t flags)
        {
            int16_t globalLoop;
            std::memcpy(&globalLoop, rec + kPOffGravityTrack + kTrackGlobalLoop, 2);

            const uint32_t slots  = Rd32(rec + kPOffGravityTrack + kTrackValues);
            const uint32_t offset = Rd32(rec + kPOffGravityTrack + kTrackValues + 4);
            if (slots && offset)
            {
                if (!FitsInBody(ctx.size, offset, slots, sizeof(M2Array<uint8_t>))) return false;
                const M2Sequence* seqs = nullptr;
                if (globalLoop == kNoGlobalLoop && ctx.header->m_sequences.m_count && ctx.header->m_sequences.m_offset)
                    seqs = reinterpret_cast<const M2Sequence*>(ctx.body + ctx.header->m_sequences.m_offset);

                for (uint32_t s = 0; s < slots; ++s)
                {
                    // Keys that stream in with a companion file are not addressed against this body; leaving
                    // them packed is correct, that file's arrival brings its own.
                    if (seqs && s < ctx.header->m_sequences.m_count && !(seqs[s].m_flags & kSeqKeysInline)) continue;

                    const uint8_t* slot = ctx.body + offset + s * sizeof(M2Array<uint8_t>);
                    const uint32_t keyCount = Rd32(slot);
                    const uint32_t keyOffset = Rd32(slot + 4);
                    if (!keyCount || !keyOffset) continue;
                    if (!FitsInBody(ctx.size, keyOffset, keyCount, 4)) return false;

                    uint8_t* keys = ctx.body + keyOffset;
                    for (uint32_t k = 0; k < keyCount; ++k)
                    {
                        uint8_t* key = keys + k * 4;
                        const float dx = static_cast<int8_t>(key[0]) * kPackedDirectionUnit;
                        const float dy = static_cast<int8_t>(key[1]) * kPackedDirectionUnit;
                        int16_t packedMag; std::memcpy(&packedMag, key + 2, 2);

                        const float planar = dx * dx + dy * dy;
                        float z = std::sqrt(planar < 1.0f ? 1.0f - planar : 0.0f);
                        float mag = packedMag * kGravityMagnitudeUnit;
                        if (mag < 0.0f) { z = -z; mag = -mag; }
                        const float scalar = -(z * mag);
                        std::memcpy(key, &scalar, 4);
                    }
                }
            }
            Wr32(rec + kPOffFlags, flags & ~kFlagCompressedGravity);
            return true;
        }

        bool NormalizeParticleRecord(const NormalizeCtx& ctx, const uint8_t* src, uint8_t* dst)
        {
            // The trailing multi-texture scroll parameters stop here: nothing downstream reads them.
            std::memmove(dst, src, kParticleStrideClient);

            const uint32_t flags = Rd32(dst + kPOffFlags);

            // A multi-layer emitter packs three layer ids into the texture slot; only the first has a home
            // in a single-texture emitter, and an id past the model's table would select nothing.
            uint16_t texId = Rd16(dst + kPOffTextureId);
            if (flags & kParticleFlagMultiTex) texId &= kPackedTextureIdMask;
            if (ctx.header->m_textures.m_count && texId >= ctx.header->m_textures.m_count) texId = 0;
            Wr16(dst + kPOffTextureId, texId);

            if ((flags & kFlagCompressedGravity) && !ExpandGravityKeys(ctx, dst, flags)) return false;
            return true;
        }

        // Camera: target record is type, fov, clip planes, then the track body; the source record has no fov
        // float (everything sits one float earlier) and carries the fov as a trailing track instead.
        constexpr uint32_t kCOffType = 0x00, kCOffFov = 0x04, kCOffFarClip = 0x08, kCOffNearClip = 0x0C,
                           kCOffBody = 0x10;
        constexpr uint32_t kCSrcFarClip = 0x04, kCSrcNearClip = 0x08, kCSrcBody = 0x0C, kCSrcFovTrack = 0x60;
        constexpr uint32_t kCBodyBytes = 0x54;
        constexpr uint32_t kSplineKeyBytes = 0x0C; // value + two tangents, value first
        constexpr float    kDefaultFov = 0.7853982f; // 45 degrees, for a source track we cannot address
        static_assert(offsetof(M2Camera, m_fieldOfView) == kCOffFov, "camera fov");
        static_assert(offsetof(M2Camera, m_positions) == kCOffBody, "camera track body");
        static_assert(kCOffBody + kCBodyBytes == kCameraStrideClient, "camera target width");
        static_assert(kCSrcBody + kCBodyBytes == kCSrcFovTrack, "camera source body");
        static_assert(kCSrcFovTrack + 0x14 == kCameraStrideModern, "camera source width");

        float FirstFovKey(const NormalizeCtx& ctx, const uint8_t* track)
        {
            int16_t globalLoop; std::memcpy(&globalLoop, track + kTrackGlobalLoop, 2);

            const uint32_t slots  = Rd32(track + kTrackValues);
            const uint32_t offset = Rd32(track + kTrackValues + 4);
            if (!slots || !offset) return kDefaultFov;
            if (!FitsInBody(ctx.size, offset, slots, sizeof(M2Array<uint8_t>))) return kDefaultFov;

            // Keyed per sequence, the first slot belongs to the first sequence, and only reaches into this
            // body when that sequence keeps its keys here rather than in a companion file.
            if (globalLoop == kNoGlobalLoop)
            {
                if (!ctx.header->m_sequences.m_count || !ctx.header->m_sequences.m_offset) return kDefaultFov;
                const auto* seqs = reinterpret_cast<const M2Sequence*>(ctx.body + ctx.header->m_sequences.m_offset);
                if (!(seqs[0].m_flags & kSeqKeysInline)) return kDefaultFov;
            }

            const uint8_t* slot = ctx.body + offset;
            const uint32_t keyCount = Rd32(slot);
            const uint32_t keyOffset = Rd32(slot + 4);
            if (!keyCount || !keyOffset) return kDefaultFov;
            if (!FitsInBody(ctx.size, keyOffset, keyCount, kSplineKeyBytes)) return kDefaultFov;

            float fov; std::memcpy(&fov, ctx.body + keyOffset, 4);
            return fov;
        }

        bool NormalizeCameraRecord(const NormalizeCtx& ctx, const uint8_t* src, uint8_t* dst)
        {
            // Read every scalar out before anything moves: at the first record dst and src are the same
            // address, so the body slide overwrites the near clip plane where it lay in the source form.
            const uint32_t type = Rd32(src + kCOffType);
            const uint32_t farClip = Rd32(src + kCSrcFarClip);
            const uint32_t nearClip = Rd32(src + kCSrcNearClip);
            const float fov = FirstFovKey(ctx, src + kCSrcFovTrack);

            std::memmove(dst + kCOffBody, src + kCSrcBody, kCBodyBytes);
            Wr32(dst + kCOffType, type);
            std::memcpy(dst + kCOffFov, &fov, 4);
            Wr32(dst + kCOffFarClip, farClip);
            Wr32(dst + kCOffNearClip, nearClip);
            return true;
        }

        using NormalizeRecordFn = bool (*)(const NormalizeCtx&, const uint8_t*, uint8_t*);

        struct RecordNormalizer
        {
            uint32_t          at;               // byte offset of the count+offset pair inside the header
            uint32_t          sourceStride;
            uint32_t          targetStride;
            uint32_t          sourceMinVersion; // below it the record already has the target shape
            NormalizeRecordFn rewrite;
            const char*       label;
        };

        const RecordNormalizer kNormalizers[] = {
            { offsetof(M2Data, m_particles), kParticleStrideModern, kParticleStrideClient, kParticleModernMinVer,
              &NormalizeParticleRecord, "particles" },
            { offsetof(M2Data, m_cameras), kCameraStrideModern, kCameraStrideClient, kCameraModernMinVer,
              &NormalizeCameraRecord, "cameras" },
        };
        constexpr uint32_t kNormalizerCount = sizeof(kNormalizers) / sizeof(kNormalizers[0]);

        /// Rewrites every source-era-wide record listed in the table to the client shape, in place.
        /// Returns false when a record (or an array nested in one) does not fit the body.
        bool NormalizeRecords(uint8_t* base, uint32_t size, M2Data* h, uint32_t (&counts)[kNormalizerCount])
        {
            const NormalizeCtx ctx{ base, size, h };
            for (uint32_t e = 0; e < kNormalizerCount; ++e)
            {
                const RecordNormalizer& entry = kNormalizers[e];
                counts[e] = 0;
                if (h->m_version < entry.sourceMinVersion) continue;
                if (entry.targetStride > entry.sourceStride) return false; // normalization only ever shrinks

                auto& array = *reinterpret_cast<M2Array<uint8_t>*>(reinterpret_cast<uint8_t*>(h) + entry.at);
                if (!array.m_count || !array.m_offset) continue;
                if (!FitsInBody(size, array.m_offset, array.m_count, entry.sourceStride)) return false;

                const uint8_t* src = base + array.m_offset;
                uint8_t*       dst = base + array.m_offset;
                for (uint32_t i = 0; i < array.m_count; ++i)
                {
                    if (!entry.rewrite(ctx, src, dst)) return false;
                    src += entry.sourceStride;
                    dst += entry.targetStride;
                }
                counts[e] = array.m_count;
            }
            return true;
        }

        // ---------------------------------------------------------------------------------- listfile
        const WxlListfileServices* ListfileService()
        {
            static std::atomic<const WxlListfileServices*> s_cached{ nullptr };
            const WxlListfileServices* s = s_cached.load(std::memory_order_acquire);
            if (s) return s;
            size_t size = 0;
            s = static_cast<const WxlListfileServices*>(wxl::services::Get(WXL_SERVICE_LISTFILE, &size));
            if (s && size >= sizeof(s->structSize) && s->structSize >= offsetof(WxlListfileServices, resolve) + sizeof(s->resolve))
                s_cached.store(s, std::memory_order_release);
            else
                s = nullptr;
            return s;
        }

        /// Spare bytes seated after the body for arrays the skin finalize rebuilds.
        constexpr uint32_t kModelReserve = 4096;
    }

    // ------------------------------------------------------------------------------------ PrepareLoad
    LoadResult PrepareLoad(CM2Shared* shared)
    {
        uint8_t* buf = reinterpret_cast<uint8_t*>(shared->m_data);
        const uint32_t size = shared->m_fileSize;
        if (!buf || size < 8 || Rd32(buf) != kTagMD21) return LoadResult::NotModern;

        const char* fail = nullptr;
        Scan s;
        uint32_t version = 0;

        if (!ScanContainer(buf, size, s)) fail = "no MD20 body in container";

        if (!fail)
        {
            version = Rd32(buf + s.bodyOff + offsetof(M2Data, m_version));
            if (Rd32(buf + s.bodyOff) != kTagMD20) fail = "body magic";
            else if (!IsAcceptedVersion(version)) fail = "inner version outside the supported source window";
        }

        if (!fail)
        {
            // A split-skeleton model (SKID, or empty bone and sequence arrays) has no home here: without the
            // .skel splice it would stand unboned. Refuse it like an absent file.
            const auto* raw = reinterpret_cast<const M2Data*>(buf + s.bodyOff);
            if ((s.skipMask & kSkipSkid) || (!raw->m_bones.m_count && !raw->m_sequences.m_count))
                fail = "split skeleton (.skel) model";
        }

        if (fail)
        {
            wxl::LogCat("M2", "'%s' modern load FAILED: %s (v=%u skips=0x%X)", shared->m_fileName, fail, version, s.skipMask);
            return LoadResult::Failed;
        }

        // Seat the flat body in a fresh block with spare room after it. The model's block is only ever freed
        // through SIMD::FreeA16 on m_data, so swapping it here is invisible to the rest of the client; every
        // offset in the body is body-relative, so the copy needs no fixing up.
        const uint32_t reserveBase = (s.bodySize + 15u) & ~15u;
        const uint32_t total = reserveBase + kModelReserve;
        uint8_t* seated = static_cast<uint8_t*>(SIMD::AllocA16(total, __FILE__, __LINE__));
        if (!seated)
        {
            wxl::LogCat("M2", "'%s' modern load FAILED: out of memory (%u bytes)", shared->m_fileName, total);
            return LoadResult::Failed;
        }
        std::memcpy(seated, buf + s.bodyOff, s.bodySize);
        std::memset(seated + s.bodySize, 0, total - s.bodySize);
        SIMD::FreeA16(buf);
        shared->m_data = reinterpret_cast<M2Data*>(seated);
        shared->m_fileSize = s.bodySize;

        auto* h = reinterpret_cast<M2Data*>(seated);
        const uint32_t bodySize = s.bodySize;

        // The stock skin chooser walks a 4-entry threshold table indexed 4 - numSkinProfiles; more profiles
        // than the client ever shipped (LDV1-era LOD counts) would underflow it.
        if (h->m_numSkinProfiles > 4)
        {
            s.skipMask |= kSkipLdv1;
            h->m_numSkinProfiles = 1; // profile 0 = full detail
        }

        const uint32_t external = FixSequences(seated, bodySize, h);
        FixMaterials(seated, bodySize, h);

        uint32_t normalized[kNormalizerCount] = {};
        if (!NormalizeRecords(seated, bodySize, h, normalized))
        {
            wxl::LogCat("M2", "'%s' modern load FAILED: record normalization rejected a record", shared->m_fileName);
            return LoadResult::Failed;
        }

        State* state = new (std::nothrow) State();
        if (!state)
        {
            wxl::LogCat("M2", "'%s' modern load FAILED: out of memory (state)", shared->m_fileName);
            return LoadResult::Failed;
        }
        state->modern = true;
        state->txidCount = s.txidCount;
        std::memcpy(state->txid, s.txid, sizeof(uint32_t) * s.txidCount);
        state->reserveBase = reserveBase;
        state->reserveCap = kModelReserve;
        shared->m_wxl = state;

        char norm[96] = {};
        size_t at = 0;
        for (uint32_t i = 0; i < kNormalizerCount && at + 1 < sizeof norm; ++i)
        {
            if (!normalized[i]) continue;
            const int wrote = std::snprintf(norm + at, sizeof norm - at, " %s=%u", kNormalizers[i].label, normalized[i]);
            if (wrote <= 0) break;
            at += static_cast<size_t>(wrote);
        }
        wxl::LogCat("M2", "'%s' read natively (v=%u txid=%u skips=0x%X extseq=%u normalized:%s)", shared->m_fileName,
                    version, s.txidCount, s.skipMask, external, norm[0] ? norm : " none");
        return LoadResult::Modern;
    }

    // -------------------------------------------------------------------------------------- FinishWalk
    void FinishWalk(CM2Shared* shared)
    {
        M2Data* h = shared->m_data;

        // Ribbons: the source exporter can emit texture / material indices past what the model declares.
        for (uint32_t i = 0; i < h->m_ribbons.m_count; ++i)
        {
            M2Ribbon& ribbon = h->m_ribbons[i];
            auto clamp = [](M2Array<uint16_t>& a, uint32_t limit)
            {
                if (!a.m_count || !limit) return;
                uint16_t* v = a.Data();
                for (uint32_t n = 0; n < a.m_count; ++n)
                    if (v[n] >= limit) v[n] = static_cast<uint16_t>(limit - 1);
            };
            clamp(ribbon.m_textureIndices, h->m_textures.m_count);
            clamp(ribbon.m_materialIndices, h->m_materials.m_count);
        }

        // Cameras: a lookup entry past the camera array is the exporter's "no camera" sentinel.
        if (h->m_cameraIndicesById.m_count)
        {
            int16_t* lookup = reinterpret_cast<int16_t*>(h->m_cameraIndicesById.Data());
            const int32_t limit = static_cast<int32_t>(h->m_cameras.m_count);
            for (uint32_t i = 0; i < h->m_cameraIndicesById.m_count; ++i)
                if (lookup[i] >= limit) lookup[i] = -1;
        }
    }

    // ------------------------------------------------------------------------------------ TextureName
    bool TextureName(CM2Shared* shared, uint32_t index, const M2Texture& texture, char* out, size_t cap)
    {
        State* state = shared->m_wxl;
        if (!state || !state->modern) return false;
        if (texture.m_type != 0) return false;                  // dynamic slots stay empty
        if (index >= state->txidCount || !state->txid[index]) return false;

        const uint32_t fileId = state->txid[index];
        const WxlListfileServices* listfile = ListfileService();
        if (!listfile)
        {
            if (wxl::LogThrottle("m2-txid-nolistfile", 1, 100000))
                wxl::LogCat("M2", "TXID %u: the listfile service (%s) is not available, solid white fallback", fileId, WXL_SERVICE_LISTFILE);
            ++state->texUnresolved;
            return false;
        }

        // resolve() returns the file's own path, or, when the listfile does not know the id or the client
        // cannot open the file, the not-found name for a texture; opening that name makes the client fall
        // back to its own missing-texture handling, which is the listfile module's contract.
        const int result = listfile->resolve(fileId, WXL_LISTFILE_TEXTURE, out, cap);
        if (result == WXL_LISTFILE_ERROR) { ++state->texUnresolved; return false; }
        if (result == WXL_LISTFILE_FOUND) ++state->texResolved; else ++state->texUnresolved;
        return true;
    }

    // -------------------------------------------------------------------------------------- UnwrapAnim
    bool UnwrapAnim(uint8_t* buffer, uint32_t& size)
    {
        constexpr uint32_t kChunkHeaderBytes = 8;
        if (!buffer || size < kChunkHeaderBytes || Rd32(buffer) != kTagAFM2) return false;
        const uint32_t payload = Rd32(buffer + 4);
        if (static_cast<uint64_t>(kChunkHeaderBytes) + payload > size) return false;
        std::memmove(buffer, buffer + kChunkHeaderBytes, payload);
        size = payload;
        return true;
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
