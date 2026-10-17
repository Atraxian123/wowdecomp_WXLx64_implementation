// wxl-equip-extension: client-side virtual M2 path table and file provider.
//
// Virtual paths encode (cmo × model × merged-geoset-filter × texture) in the filename so the
// engine's model hash table sees a distinct cache key for every unique combination. The host
// serve hook is bypassed; bytes are served directly from an in-process table. Collection skins are
// also prefiltered here so synchronous skin loads receive the same geoset-trimmed data.
//
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "Engine/Source/WXLEquip/VirtualPath.hpp"

#include "Engine/Source/WXL/Storage.h"
#include "Engine/Source/WXLEquip/M2Format.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wxl::scripts::equipextension
{
    namespace
    {
        static bool EquipLogEnabled() noexcept
        {
            static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
                const char* env = std::getenv("WXL_EQUIP_LOG");
                if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                    return 1;

#pragma warning(suppress: 4996)
                FILE* flag = std::fopen("WarcraftXL_equip.log.enable", "rb");
                if (!flag) return 0;
                std::fclose(flag);
                return 1;
            }();
            return enabled != 0;
        }

        static void VPathLog(const char* fmt, ...) noexcept
        {
            if (!EquipLogEnabled()) return;
#pragma warning(suppress: 4996)
            FILE* f = std::fopen("WarcraftXL_equip.log", "a");
            if (!f) return;
            va_list ap; va_start(ap, fmt);
            std::vfprintf(f, fmt, ap);
            va_end(ap);
            std::fputc('\n', f);
            std::fclose(f);
        }
        // Guards every table below. The injected build documented these as main-thread-only, but the
        // client opens model files from loader threads, and VirtualProvide runs inside that open.
        // Recursive because a populate reads real files through SFile::OpenEx, which re-enters
        // VirtualProvide on the same thread.
        std::recursive_mutex g_vpMutex;

        // virtual path -> raw file bytes (both .mdx and 00.skin entries per model).
        std::unordered_map<std::string, std::vector<uint8_t>> g_virtualBytes;

        // cmo -> list of virtual paths it owns, for O(n) cleanup on eviction.
        std::unordered_map<void*, std::vector<std::string>> g_cmoVPaths;

        // REAL (unmangled, normalized) archive path -> patched bytes. Unlike g_virtualBytes,
        // these are served under the model's own real name and apply to every loader, not just
        // paths this module itself constructed. See VPathPopulateGlobal. Non-evictable entries
        // (the default) are permanent for the life of the process, same as before; entries
        // registered with evictable=true (creatures, weapons) can be reclaimed by EvictOverBudget
        // once their pool's combined size exceeds that pool's configured budget -- see
        // g_evictablePools below.
        std::unordered_map<std::string, std::vector<uint8_t>> g_globalOverrides;

        // Lazy-bake resolvers, tried in registration order on a g_globalOverrides miss. See
        // VPathRegisterLazyResolver's doc comment in VirtualPath.hpp. Registration happens once per
        // consumer at static-init time.
        std::vector<VPathLazyResolver> g_lazyResolvers;

        // ─── Evictable-entry LRU bookkeeping ──────────────────────────────────
        //
        // Only entries registered via VPathPopulateGlobal(..., evictable=true) are tracked here at
        // all -- non-evictable entries never appear in these tables and are never touched by
        // EvictOverBudget, so this whole mechanism is opt-in per bake, not global. A "group" is
        // everything one VPathPopulateGlobal call registered (the .m2 key, plus the .skin key if a
        // skin was present) -- evicted and touched as a single unit, since serving the .m2 for a
        // displayId without its matching .skin (or vice versa) after a partial eviction would produce
        // mismatched geometry/texture data.
        //
        // Bookkeeping is partitioned per evictionPool (e.g. "Creature", "Weapon"): each pool has its
        // own groups table and its own running byte total, budgeted independently via that pool's
        // own "Max<Pool>CacheMB" ini key (see GetEvictableByteBudget). This is what keeps one
        // feature's cache pressure from evicting another's entries -- a flood of newly-equipped
        // weapons can fill the weapon pool without touching a single creature entry, and vice versa.
        struct EvictableGroup
        {
            std::vector<std::string> keys; // table keys this group owns (.m2, optionally .skin)
            uint64_t bytes = 0;            // combined size of those keys' bytes in g_globalOverrides
            uint64_t tick  = 0;            // last-touched tick; lower = evicted first
        };
        struct EvictablePool
        {
            std::unordered_map<std::string, EvictableGroup> groups; // groupId (the .m2 vKey) -> group
            uint64_t bytesTotal = 0;
        };
        std::unordered_map<std::string, EvictablePool> g_evictablePools; // pool name -> pool state

        // Any owned table key (the .m2 groupId itself, or its .skin key) -> (pool, groupId). Both
        // keys of a group map to the same groupId (the .m2 vKey), so a touch on either the model or
        // the skin key finds the same group.
        struct EvictableKeyLoc { std::string pool; std::string groupId; };
        std::unordered_map<std::string, EvictableKeyLoc> g_evictableKeyLoc;
        uint64_t g_evictTickCounter = 0;

        // Bumps an evictable group's LRU tick, if tableKey belongs to one. A no-op for any key that
        // isn't tracked -- i.e. every non-evictable entry, and every "_wxl_"/g_virtualBytes key,
        // which never go through this path at all.
        static void TouchEvictableGroup(const std::string& tableKey)
        {
            auto locIt = g_evictableKeyLoc.find(tableKey);
            if (locIt == g_evictableKeyLoc.end()) return;
            auto poolIt = g_evictablePools.find(locIt->second.pool);
            if (poolIt == g_evictablePools.end()) return;
            auto git = poolIt->second.groups.find(locIt->second.groupId);
            if (git != poolIt->second.groups.end()) git->second.tick = ++g_evictTickCounter;
        }

        // "Max<Pool>CacheMB" from WXLExtendedEquipment.ini's [Memory] section, cached per pool name
        // after its first read (the ini isn't expected to change mid-session). <= 0 (including
        // "missing" -- the default below) means no cap is enforced for that pool: entries are still
        // tracked and touched, they're just never actually evicted. Default chosen generously; tune
        // per deployment, per pool, via the ini.
        static uint64_t GetEvictableByteBudget(const std::string& pool)
        {
            static std::unordered_map<std::string, uint64_t> cache;
            auto it = cache.find(pool);
            if (it != cache.end()) return it->second;

            char key[64];
            std::snprintf(key, sizeof(key), "Max%sCacheMB", pool.c_str());
            const int mb = WxlIniGetInt("Memory", key, 512);
            const uint64_t budget = (mb <= 0) ? 0 : static_cast<uint64_t>(mb) * 1024ull * 1024ull;
            cache.emplace(pool, budget);
            return budget;
        }

        // Evicts least-recently-touched evictable groups within one pool until that pool's byte
        // total is back under its own budget (or there's nothing left in it that's safe to evict).
        // Never touches any other pool's groups. justBakedGroupId is the group RegisterEvictableGroup
        // just finished inserting -- if it turns out to be both the LRU victim AND the only evictable
        // group in this pool, it's kept rather than evicted: the budget is a steady-state target, not
        // a hard per-bake ceiling, and evicting the entry that was just requested would just force an
        // immediate rebake on the very next load of it.
        static void EvictOverBudget(const std::string& pool, const std::string& justBakedGroupId)
        {
            const uint64_t budget = GetEvictableByteBudget(pool);
            if (budget == 0) return; // uncapped for this pool

            auto poolIt = g_evictablePools.find(pool);
            if (poolIt == g_evictablePools.end()) return;
            EvictablePool& poolState = poolIt->second;

            while (poolState.bytesTotal > budget)
            {
                std::string victim;
                uint64_t victimTick = UINT64_MAX;
                for (const auto& [groupId, group] : poolState.groups)
                {
                    if (group.tick < victimTick) { victimTick = group.tick; victim = groupId; }
                }
                if (victim.empty()) break; // nothing tracked in this pool at all

                if (victim == justBakedGroupId && poolState.groups.size() == 1) break;

                auto git = poolState.groups.find(victim);
                for (const std::string& key : git->second.keys)
                {
                    g_globalOverrides.erase(key);
                    g_evictableKeyLoc.erase(key);
                }
                poolState.bytesTotal -= git->second.bytes;
                VPathLog("  VPathPopulateGlobal: evicted '%s' from pool '%s' (%llu bytes, pool total "
                         "now %llu/%llu bytes)",
                         victim.c_str(), pool.c_str(),
                         static_cast<unsigned long long>(git->second.bytes),
                         static_cast<unsigned long long>(poolState.bytesTotal),
                         static_cast<unsigned long long>(budget));
                poolState.groups.erase(git);
            }
        }

        // Records a freshly baked (vKey, vSkin?) pair as one evictable unit in the given pool and
        // enforces that pool's budget. Called only after both keys are already in g_globalOverrides --
        // bytes are measured from there rather than passed in, so this can't drift out of sync with
        // what was actually stored.
        static void RegisterEvictableGroup(const std::string& pool, const std::string& vKey,
                                           const std::string& vSkin, bool hasSkin)
        {
            EvictableGroup group;
            group.keys.push_back(vKey);
            group.bytes = g_globalOverrides[vKey].size();
            if (hasSkin)
            {
                group.keys.push_back(vSkin);
                group.bytes += g_globalOverrides[vSkin].size();
            }
            group.tick = ++g_evictTickCounter;

            EvictablePool& poolState = g_evictablePools[pool];
            poolState.bytesTotal += group.bytes;
            for (const std::string& key : group.keys)
                g_evictableKeyLoc[key] = EvictableKeyLoc{ pool, vKey };
            poolState.groups[vKey] = std::move(group); // overwrite is fine: same-pair rebake, same size class

            EvictOverBudget(pool, vKey);
        }

        // ─── Key building ─────────────────────────────────────────────────────

        // Sorts ids[0..n-1] ascending in place (insertion sort; n <= 16).
        static void SortIds(uint16_t* ids, uint32_t n) noexcept
        {
            for (uint32_t i = 1; i < n; ++i)
            {
                uint16_t k = ids[i];
                int32_t j = static_cast<int32_t>(i) - 1;
                while (j >= 0 && ids[j] > k) { ids[j + 1] = ids[j]; --j; }
                ids[j + 1] = k;
            }
        }

        // Writes a uint16 as decimal into *q, advancing q. Returns new q.
        static char* WriteU16(char* q, char* end, uint16_t v) noexcept
        {
            char tmp[8]; int len = 0;
            if (v == 0) { tmp[len++] = '0'; }
            else { while (v) { tmp[len++] = '0' + (v % 10); v /= 10; } }
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        // Writes a uintptr_t as lowercase hex into *q, advancing q. Returns new q.
        static char* WriteHex(char* q, char* end, uintptr_t v) noexcept
        {
            char tmp[9]; int len = 0;
            const char* digits = "0123456789abcdef";
            do { tmp[len++] = digits[v & 0xF]; v >>= 4; } while (v);
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        static char LowerAscii(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }

        static uint32_t HashCString(const char* s) noexcept
        {
            uint32_t h = 2166136261u;
            if (!s) return h;
            while (*s)
            {
                h ^= static_cast<uint8_t>(LowerAscii(*s++));
                h *= 16777619u;
            }
            return h;
        }

        // Builds the virtual .m2 key (sortedIds must already be sorted ascending).
        // Format: <stem>_wxl_<id0>_<id1>..._tex_<texbasename>[_mat<hash>][_grp<hex>]_cmo<hex>.m2  (all lowercase)
        // The engine normalises all paths to lowercase and uses .m2; keys must match that form.
        static size_t BuildKey(char* out, size_t outSz, void* cmo,
                                const char* realMdxPath,
                                const uint16_t* sortedIds, uint32_t idCount,
                                const char* texPath,
                                const char* materialPatchSpec,
                                uint32_t variantKey) noexcept
        {
            if (!out || outSz == 0) return 0;
            char* q   = out;
            char* end = out + outSz - 1; // reserve one byte for null

            // Copy the stem (path without extension) — lowercase.
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            const char* stemEnd = lastDot ? lastDot : realMdxPath + std::strlen(realMdxPath);
            for (const char* p = realMdxPath; p < stemEnd && q < end; ) *q++ = LowerAscii(*p++);

            // _wxl_
            for (const char* s = "_wxl_"; *s && q < end; ) *q++ = *s++;

            // Sorted geoset IDs separated by '_'.
            for (uint32_t i = 0; i < idCount && q < end; ++i)
            {
                if (i > 0 && q < end) *q++ = '_';
                q = WriteU16(q, end, sortedIds[i]);
            }

            // _tex_<texbasename> (basename = filename without path prefix or extension) — lowercase.
            if (texPath && *texPath)
            {
                const char* base = texPath;
                for (const char* p = texPath; *p; ++p)
                    if (*p == '\\' || *p == '/') base = p + 1;
                const char* dot = nullptr;
                for (const char* p = base; *p; ++p) if (*p == '.') dot = p;
                const char* baseEnd = dot ? dot : base + std::strlen(base);

                for (const char* s = "_tex_"; *s && q < end; ) *q++ = *s++;
                for (const char* p = base; p < baseEnd && q < end; ) *q++ = LowerAscii(*p++);
            }

            if (materialPatchSpec && *materialPatchSpec)
            {
                for (const char* s = "_mat"; *s && q < end; ) *q++ = *s++;
                q = WriteHex(q, end, HashCString(materialPatchSpec));
            }

            if (variantKey != 0)
            {
                for (const char* s = "_grp"; *s && q < end; ) *q++ = *s++;
                q = WriteHex(q, end, variantKey);
            }

            // _cmo<hex> (lowercase)
            for (const char* s = "_cmo"; *s && q < end; ) *q++ = *s++;
            q = WriteHex(q, end, reinterpret_cast<uintptr_t>(cmo));

            // .m2  (engine requests all paths with .m2 extension, not .mdx)
            for (const char* s = ".m2"; *s && q < end; ) *q++ = *s++;

            *q = '\0';
            return static_cast<size_t>(q - out);
        }

        // ─── File reading ─────────────────────────────────────────────────────

        // Reads all bytes of a game archive file via the existing IO wrappers.
        // Uses kOpenWholeFile so the handle buffer holds the full content immediately.
        static bool ReadGameFile(const char* path, std::vector<uint8_t>& out) noexcept
        {
            // Goes through the client's own SFile::OpenEx (installed by wxl::equip::Init), so loose
            // files and archives resolve exactly as for the game itself. Our own provider sees this
            // open too and must not, and does not, claim a real path.
            out.clear();
            return wxl::storage::ReadNative(path, out) && !out.empty();
        }

        // Derives the real skin path: strip extension (.m2 or .mdx), append 00.skin.
        static void RealSkinPath(char* out, size_t outSz, const char* realMdxPath) noexcept
        {
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            size_t stemLen = lastDot ? static_cast<size_t>(lastDot - realMdxPath)
                                     : std::strlen(realMdxPath);
            if (stemLen >= outSz) stemLen = outSz - 1;
            std::memcpy(out, realMdxPath, stemLen);
            const char* suffix = "00.skin";
            size_t rem = outSz - stemLen - 1;
            size_t suffLen = std::strlen(suffix);
            if (suffLen > rem) suffLen = rem;
            std::memcpy(out + stemLen, suffix, suffLen);
            out[stemLen + suffLen] = '\0';
        }

        // Derives the virtual skin path from a virtual .m2 key: strip .m2, append 00.skin.
        static void VirtualSkinPath(char* out, size_t outSz, const char* virtualM2Key) noexcept
        {
            RealSkinPath(out, outSz, virtualM2Key); // same operation: strip extension, append 00.skin
        }

        // Writes a uint32 as decimal into *q, advancing q. Returns new q. (itemDisplayId is a
        // full 32-bit value; WriteU16 above tops out at 16 bits so it can't be reused here.)
        static char* WriteU32Dec(char* q, char* end, uint32_t v) noexcept
        {
            char tmp[10]; int len = 0;
            if (v == 0) { tmp[len++] = '0'; }
            else { while (v) { tmp[len++] = static_cast<char>('0' + (v % 10)); v /= 10; } }
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        // Builds the per-itemDisplayId virtual key for VPathPopulateGlobal: <stem>_<itemDisplayId>.m2
        // normPath must already be normalized (lowercase, .m2 extension) by NormalizeRealPath.
        static void BuildGlobalVirtualKey(char* out, size_t outSz, const char* normPath,
                                          uint32_t itemDisplayId) noexcept
        {
            if (!out || outSz == 0) return;
            char* q   = out;
            char* end = out + outSz - 1; // reserve one byte for null

            const char* lastDot = nullptr;
            for (const char* p = normPath; *p; ++p) if (*p == '.') lastDot = p;
            const char* stemEnd = lastDot ? lastDot : normPath + std::strlen(normPath);
            for (const char* p = normPath; p < stemEnd && q < end; ) *q++ = *p++;

            if (q < end) *q++ = '_';
            q = WriteU32Dec(q, end, itemDisplayId);

            for (const char* p = (lastDot ? lastDot : ".m2"); *p && q < end; ) *q++ = *p++;

            *q = '\0';
        }

        static uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t off) noexcept
        {
            return static_cast<uint16_t>(bytes[off] | (bytes[off + 1] << 8));
        }

        static uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t off) noexcept
        {
            return static_cast<uint32_t>(bytes[off])
                 | (static_cast<uint32_t>(bytes[off + 1]) << 8)
                 | (static_cast<uint32_t>(bytes[off + 2]) << 16)
                 | (static_cast<uint32_t>(bytes[off + 3]) << 24);
        }

        static void WriteU32(std::vector<uint8_t>& bytes, size_t off, uint32_t value) noexcept
        {
            bytes[off + 0] = static_cast<uint8_t>(value);
            bytes[off + 1] = static_cast<uint8_t>(value >> 8);
            bytes[off + 2] = static_cast<uint8_t>(value >> 16);
            bytes[off + 3] = static_cast<uint8_t>(value >> 24);
        }

        static void Align4(std::vector<uint8_t>& bytes)
        {
            while (bytes.size() & 3u) bytes.push_back(0);
        }

        static uint32_t ParseU32Span(const char* begin, const char* end, uint32_t fallback) noexcept
        {
            if (!begin || !end || begin >= end) return fallback;
            const char* p = begin;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p >= end || *p < '0' || *p > '9') return fallback;
            uint32_t value = 0;
            while (p < end && *p >= '0' && *p <= '9')
                value = value * 10u + static_cast<uint32_t>(*p++ - '0');
            return value;
        }

        struct MaterialPatch
        {
            uint32_t textureType = 0xffffffffu;
            const char* path = nullptr;
            size_t pathLen = 0;
        };

        // NOTE: this used to also carry layer/batch/section-list fields and a hide/edgefade mode,
        // driven by the sidecar CSV's Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs,
        // and TargetBatchIndexes columns. Those columns are no longer read by the loader at all
        // (see LoadMaterialSidecarFile in EquipExtension.cpp), so the hide-by-batch and edgefade
        // machinery that depended on them (BatchMatchesPatch, BatchLooksLikeEdgeFade,
        // RemoveHiddenBatches, IsHidePath, ParseNumberList, ContainsU16, and the batch/section
        // fields on MaterialPatch) has been removed along with them -- there is no longer any data
        // source to select which batches such a patch would apply to. What remains is exactly the
        // TextureType-keyed value-baking behavior: each spec entry claims every texture-unit record
        // of one declared TextureType and repoints it at one baked path.

        static uint32_t ParseMaterialPatchSpec(const char* spec,
                                               MaterialPatch* out,
                                               uint32_t outCount) noexcept
        {
            if (!spec || !*spec || !out || outCount == 0) return 0;

            uint32_t count = 0;
            const char* part = spec;
            while (*part && count < outCount)
            {
                const char* end = part;
                while (*end && *end != '|') ++end;

                const char* eq = part;
                while (eq < end && *eq != '=') ++eq;

                if (eq < end)
                {
                    MaterialPatch p = {};
                    p.textureType = ParseU32Span(part, eq, 0xffffffffu);
                    p.path = eq + 1;
                    while (p.path < end && (*p.path == ' ' || *p.path == '\t')) ++p.path;
                    const char* pathEnd = end;
                    while (pathEnd > p.path && (pathEnd[-1] == ' ' || pathEnd[-1] == '\t')) --pathEnd;
                    p.pathLen = static_cast<size_t>(pathEnd - p.path);
                    if (p.textureType != 0xffffffffu && p.pathLen > 0)
                        out[count++] = p;
                }

                if (!*end) break;
                part = end + 1;
            }
            return count;
        }

        static void PatchTargetedMaterialTextures(std::vector<uint8_t>& modelBytes,
                                                  const char* materialPatchSpec) noexcept
        {
            if (!materialPatchSpec || !*materialPatchSpec) return;

            MaterialPatch patches[16];
            const uint32_t patchCount = ParseMaterialPatchSpec(materialPatchSpec, patches, 16);
            if (!patchCount) return;

            namespace fmt = wxl::structure::m2;
            if (modelBytes.size() < sizeof(fmt::M2Header)) return;
            if (ReadU32(modelBytes, 0x00) != fmt::kMagicMD20) return;

            // Texture value patching is keyed purely on TextureType: for each patch, every texture
            // record on the model already declaring that type gets promoted to HARDCODED and
            // repointed at the patch's path, in place. This mirrors what the client itself does
            // with a model's native WEAPON_BLADE/OBJECT_SKIN records -- multiple records of the
            // same declared type already resolve to a single .blp regardless of how many exist, so
            // writing one path into all of them changes nothing structurally. Two patches that
            // target the same TextureType on one model: the first to run claims every record of
            // that type (converting them to HARDCODED), so the second finds nothing left to match
            // and is skipped -- deliberate, not a bug; a model that legitimately needs two distinct
            // simultaneous textures of one declared type is not representable here.
            uint32_t patchedRecords = 0;
            for (uint32_t pi = 0; pi < patchCount; ++pi)
            {
                uint32_t texCount = ReadU32(modelBytes, 0x50);
                uint32_t texOfs = ReadU32(modelBytes, 0x54);
                constexpr uint32_t kTexStride = sizeof(fmt::M2Texture);
                if (texOfs > modelBytes.size()) continue;
                if (texCount > (modelBytes.size() - texOfs) / kTexStride) continue;

                uint32_t matches = 0;
                for (uint32_t i = 0; i < texCount; ++i)
                {
                    const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                    if (ReadU32(modelBytes, rec + 0x00) == patches[pi].textureType) ++matches;
                }
                if (matches == 0)
                {
                    VPathLog("  VPathPopulate: material patch textureType=%u has no matching "
                             "texture record on this model, skipped spec='%s'",
                             patches[pi].textureType, materialPatchSpec);
                    continue;
                }

                Align4(modelBytes);
                const uint32_t pathOfs = static_cast<uint32_t>(modelBytes.size());
                modelBytes.insert(modelBytes.end(), patches[pi].path, patches[pi].path + patches[pi].pathLen);
                modelBytes.push_back(0);

                // The insert above only appends bytes past the texture table -- texOfs/texCount
                // are unaffected -- but re-read for clarity since modelBytes may have reallocated.
                texOfs = ReadU32(modelBytes, 0x54);
                texCount = ReadU32(modelBytes, 0x50);
                for (uint32_t i = 0; i < texCount; ++i)
                {
                    const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                    if (ReadU32(modelBytes, rec + 0x00) != patches[pi].textureType) continue;
                    WriteU32(modelBytes, rec + 0x00, fmt::kTexTypeHardcoded);
                    WriteU32(modelBytes, rec + 0x08, static_cast<uint32_t>(patches[pi].pathLen + 1));
                    WriteU32(modelBytes, rec + 0x0C, pathOfs);
                }
                patchedRecords += matches;
                VPathLog("  VPathPopulate: material patch textureType=%u -> HARDCODED records=%u "
                         "path='%.*s'", patches[pi].textureType, matches,
                         static_cast<int>(patches[pi].pathLen), patches[pi].path);
            }
            if (patchedRecords)
            {
                VPathLog("  VPathPopulate: material patch total records=%u spec='%s'",
                         patchedRecords, materialPatchSpec);
            }
        }

        static void PatchReplaceableTextureTypes(std::vector<uint8_t>& modelBytes,
                                                 const char* texPath) noexcept
        {
            namespace fmt = wxl::structure::m2;

            if (modelBytes.size() < sizeof(fmt::M2Header)) return;
            if (ReadU32(modelBytes, 0x00) != fmt::kMagicMD20) return;

            uint32_t texCount = ReadU32(modelBytes, 0x50);
            uint32_t texOfs   = ReadU32(modelBytes, 0x54);
            constexpr uint32_t kTexStride = sizeof(fmt::M2Texture);
            if (texOfs > modelBytes.size()) return;
            if (texCount > (modelBytes.size() - texOfs) / kTexStride) return;

            const bool hasTexPath = texPath && *texPath;
            if (!hasTexPath) return; // nothing to bake; OBJECT_SKIN records are left as-is

            // OBJECT_SKIN only. WEAPON_BLADE is a distinct, differently-purposed slot and is never
            // touched here -- it is left exactly as the model declares it, so the only way it ever
            // gets a texture is an explicit sidecar TextureType=3 row via
            // PatchTargetedMaterialTextures. A record that arrived already HARDCODED (almost always
            // a particle emitter's own baked-in texture, authored directly into the base .m2 and
            // never meant to be replaceable) is never a candidate either, since matching here is
            // purely "is this OBJECT_SKIN right now".
            uint32_t patchedObjectSkin = 0;
            uint32_t promoted[128];
            uint32_t promotedCount = 0;
            for (uint32_t i = 0; i < texCount; ++i)
            {
                const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                const uint32_t texType = ReadU32(modelBytes, rec + 0x00);
                if (texType == fmt::kTexTypeObjectSkin)
                {
                    if (promotedCount < 128) promoted[promotedCount++] = i;
                    ++patchedObjectSkin;
                }
            }

            if (promotedCount > 0)
            {
                const size_t texLen = std::strlen(texPath);
                if (modelBytes.size() + texLen + 1 <= 0xffffffffu)
                {
                    const uint32_t pathOfs = static_cast<uint32_t>(modelBytes.size());
                    modelBytes.insert(modelBytes.end(), texPath, texPath + texLen);
                    modelBytes.push_back(0);

                    // Append-only insert; texOfs/texCount are unaffected but re-read since
                    // modelBytes may have reallocated.
                    texOfs = ReadU32(modelBytes, 0x54);
                    for (uint32_t k = 0; k < promotedCount; ++k)
                    {
                        const size_t rec = static_cast<size_t>(texOfs) +
                                           static_cast<size_t>(promoted[k]) * kTexStride;
                        WriteU32(modelBytes, rec + 0x00, fmt::kTexTypeHardcoded);
                        WriteU32(modelBytes, rec + 0x08, static_cast<uint32_t>(texLen + 1));
                        WriteU32(modelBytes, rec + 0x0C, pathOfs);
                    }
                }
            }

            if (patchedObjectSkin)
            {
                VPathLog("  VPathPopulate: promoted replaceable OBJECT_SKIN texture types=%u -> HARDCODED",
                         patchedObjectSkin);
            }
        }

        static bool ContainsId(const uint16_t* ids, uint32_t count, uint16_t value) noexcept
        {
            for (uint32_t i = 0; i < count; ++i)
                if (ids[i] == value) return true;
            return false;
        }

        static void ApplySkinByteFilter(std::vector<uint8_t>& skinBytes,
                                        const uint16_t* geoIds,
                                        uint32_t geoCount) noexcept
        {
            if (geoCount == 0 || skinBytes.size() < 0x2C) return;
            if (std::memcmp(skinBytes.data(), "SKIN", 4) != 0) return;

            const uint32_t rawIndexCount = ReadU32(skinBytes, 0x0C);
            const uint32_t rawIndexOfs   = ReadU32(skinBytes, 0x10);
            const uint32_t submeshCount  = ReadU32(skinBytes, 0x1C);
            const uint32_t submeshOfs    = ReadU32(skinBytes, 0x20);
            if (rawIndexOfs > skinBytes.size() || submeshOfs > skinBytes.size()) return;
            if (rawIndexCount > (skinBytes.size() - rawIndexOfs) / sizeof(uint16_t)) return;
            if (submeshCount > (skinBytes.size() - submeshOfs) / 0x30) return;

            uint32_t kept = 0;
            uint32_t zeroed = 0;
            for (uint32_t si = 0; si < submeshCount; ++si)
            {
                const size_t sub = submeshOfs + si * 0x30;
                const uint16_t sectionId = ReadU16(skinBytes, sub + 0x00);
                const uint16_t level = ReadU16(skinBytes, sub + 0x02);
                const uint16_t indexStart = ReadU16(skinBytes, sub + 0x08);
                const uint16_t indexCount = ReadU16(skinBytes, sub + 0x0A);
                if (indexCount == 0) continue;

                uint32_t fullIndexStart = (static_cast<uint32_t>(level) << 16) | indexStart;
                if (fullIndexStart > rawIndexCount || indexCount > rawIndexCount - fullIndexStart)
                {
                    fullIndexStart = indexStart;
                    if (fullIndexStart > rawIndexCount || indexCount > rawIndexCount - fullIndexStart) continue;
                }

                if (ContainsId(geoIds, geoCount, sectionId))
                {
                    ++kept;
                    continue;
                }

                std::memset(skinBytes.data() + rawIndexOfs + fullIndexStart * sizeof(uint16_t),
                            0,
                            indexCount * sizeof(uint16_t));
                ++zeroed;
            }
            VPathLog("  VPathPopulate: prefiltered skin kept=%u zeroed=%u", kept, zeroed);
        }

        // Produces a lowercase .m2 path from a (possibly mixed-case, .mdx-extension) DBC path.
        // The host stores loose files as lowercase .m2; ReadGameFile must use that form.
        static void NormalizeRealPath(char* out, size_t outSz, const char* src) noexcept
        {
            char* dst  = out;
            char* dend = out + outSz - 1;
            while (*src && dst < dend) *dst++ = LowerAscii(*src++);
            *dst = '\0';
            // Replace trailing .mdx → .m2 (DBC uses .mdx; host stores as .m2).
            char* lastDot = nullptr;
            for (char* p = out; *p; ++p) if (*p == '.') lastDot = p;
            if (lastDot && std::strcmp(lastDot, ".mdx") == 0)
                { lastDot[1] = 'm'; lastDot[2] = '2'; lastDot[3] = '\0'; }
        }

        // ─── Client-side provider ─────────────────────────────────────────────

        static bool VirtualProvide(const char* name, std::vector<uint8_t>& out)
        {
        std::lock_guard<std::recursive_mutex> vpLock(g_vpMutex);
            if (!name) return false;

            // Global overrides apply to every loader, not just our own mangled paths, so this
            // has to run before the "_wxl_" short-circuit below. Guarded on g_lazyResolvers too:
            // with eager preregister off (see WxlIniGetBool), g_globalOverrides can legitimately
            // still be empty here even though there's real work for a resolver to do on a miss.
            if (!g_globalOverrides.empty() || !g_lazyResolvers.empty())
            {
                char norm[264];
                NormalizeRealPath(norm, sizeof(norm), name);
                auto git = g_globalOverrides.find(norm);

                // Not baked yet -- this is the genuine "asked for it right now, bake it right now"
                // moment: earlier than this, nothing has requested the bytes yet (wasted work if
                // baked eagerly and never actually needed); any later than this, the loader has
                // already been handed a miss. Give each registered resolver a chance to decode norm
                // into an id it recognizes and bake it on the spot, stopping at the first one that
                // does. Skipped entirely once the table already has this exact key -- e.g. eager
                // preload already covered it, or a previous miss already lazily baked it.
                if (git == g_globalOverrides.end())
                {
                    for (VPathLazyResolver resolver : g_lazyResolvers)
                    {
                        if (!resolver || !resolver(norm)) continue;
                        git = g_globalOverrides.find(norm);
                        if (git != g_globalOverrides.end()) break;
                    }
                }

                if (git != g_globalOverrides.end())
                {
                    TouchEvictableGroup(norm); // no-op for non-evictable (e.g. weapon) entries
                    VPathLog("  VirtualProvide(global): '%s' -> %zu bytes", name, git->second.size());
                    out = git->second;
                    return true;
                }
                // Diagnostic only: a miss here previously produced no log output at all, making it
                // impossible to tell "client never requested this path" apart from "requested it but
                // the normalized form didn't match what was registered". Only logs for weapon paths
                // specifically to avoid flooding the log with every unrelated model load.
                if (std::strstr(norm, "weapon"))
                    VPathLog("  VirtualProvide(global): MISS '%s' (normalized '%s'), table has %zu entries",
                             name, norm, g_globalOverrides.size());
            }

            if (!std::strstr(name, "_wxl_")) return false;
            VPathLog("  VirtualProvide: '%s'", name);
            auto it = g_virtualBytes.find(name);
            if (it == g_virtualBytes.end())
            {
                VPathLog("  VirtualProvide: NOT FOUND (table has %zu entries)", g_virtualBytes.size());
                return false;
            }
            VPathLog("  VirtualProvide: HIT (%zu bytes)", it->second.size());
            out = it->second;
            return true;
        }

    }

    // ─── Public API ───────────────────────────────────────────────────────────

    void VPathRegister()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        wxl::storage::RegisterProvider(&VirtualProvide);
    }

    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath,
                         uint32_t variantKey,
                         const char* materialPatchSpec)
    {
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);
        return BuildKey(out, outSz, cmo, realMdxPath, sorted, n, texPath,
                        materialPatchSpec, variantKey);
    }

    bool VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath,
                       uint32_t variantKey,
                       const char* materialPatchSpec)
    {
        std::lock_guard<std::recursive_mutex> vpLock(g_vpMutex);
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);

        // Build virtual .mdx key.
        char vMdx[264];
        if (!BuildKey(vMdx, sizeof(vMdx), cmo, realMdxPath, sorted, n, texPath,
                      materialPatchSpec, variantKey)) return false;

        // No-op if already populated (same permutation on a re-equip cycle).
        if (g_virtualBytes.count(vMdx)) return true;

        // Build virtual .skin key.
        char vSkin[264];
        VirtualSkinPath(vSkin, sizeof(vSkin), vMdx);

        // Normalise the real path for host I/O: lowercase, .m2 extension.
        // DBC paths are mixed-case .mdx; the host stores loose files as lowercase .m2.
        char normPath[264];
        NormalizeRealPath(normPath, sizeof(normPath), realMdxPath);

        // Read real .m2 bytes (via normalized path).
        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(normPath, mdxBytes))
        {
            VPathLog("  VPathPopulate: mdx READ FAILED '%s'", normPath);
            return false;
        }
        VPathLog("  VPathPopulate: mdx '%s' -> %zu bytes", normPath, mdxBytes.size());

        // Read real 00.skin bytes.
        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), normPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent for some models; that is OK
        if (!skinBytes.empty())
            ApplySkinByteFilter(skinBytes, sorted, n);

        // Material-CSV texture-value patching runs BEFORE the base ModelTexture_N bake so an
        // explicit sidecar row claims and HARDCODEs its own texture-unit records first, whatever
        // TextureType it declares -- PatchTargetedMaterialTextures keys purely on the sidecar's
        // TextureType value, it is not limited to any particular type. PatchReplaceableTextureTypes
        // below only ever matches records still declaring OBJECT_SKIN(2) -- its own single fixed
        // type, unrelated to the sidecar -- so a sidecar row that happens to target TextureType=2
        // is already HARDCODED and invisible to it by the time it runs: the CSV value wins instead
        // of being silently pre-empted by the base Texture column, which is what the old (reverse)
        // ordering did to any such row. WEAPON_BLADE(3) is never touched by
        // PatchReplaceableTextureTypes at all -- an unclaimed WEAPON_BLADE record stays exactly
        // that unless a sidecar TextureType=3 row claims it, so a missing sidecar row for a
        // weapon-blade slot shows up as a visibly unresolved texture rather than being silently
        // filled in. This only ever touches modelBytes (the model's own texture-unit table), not
        // skinBytes, so it no longer needs a skin file to be present at all.
        PatchTargetedMaterialTextures(mdxBytes, materialPatchSpec);
        PatchReplaceableTextureTypes(mdxBytes, texPath);

        VPathLog("  VPathPopulate: skin '%s' -> %zu bytes", rSkin, skinBytes.size());
        VPathLog("  VPathPopulate: vMdx='%s'", vMdx);
        VPathLog("  VPathPopulate: vSkin='%s'", vSkin);

        // Store in table.
        g_virtualBytes.emplace(vMdx,  std::move(mdxBytes));
        if (!skinBytes.empty())
            g_virtualBytes.emplace(vSkin, std::move(skinBytes));

        // Register both under cmo for cleanup.
        auto& paths = g_cmoVPaths[cmo];
        paths.emplace_back(vMdx);
        if (g_virtualBytes.count(vSkin)) paths.emplace_back(vSkin);
        return true;
    }

    void VPathEvictCmo(void* cmo)
    {
        std::lock_guard<std::recursive_mutex> vpLock(g_vpMutex);
        auto it = g_cmoVPaths.find(cmo);
        if (it == g_cmoVPaths.end()) return;
        for (const auto& path : it->second)
            g_virtualBytes.erase(path);
        g_cmoVPaths.erase(it);
    }

    bool VPathPopulateGlobal(const char* realMdxPath, uint32_t itemDisplayId,
                             const char* texPath, const char* materialPatchSpec,
                             const uint16_t* geoIds, uint32_t geoCount, bool evictable,
                             char* outVirtualPath, size_t outVirtualPathSz,
                             const char* evictionPool)
    {
        std::lock_guard<std::recursive_mutex> vpLock(g_vpMutex);
        if (!realMdxPath || !*realMdxPath) return false;
        const bool hasTexPath = texPath && *texPath;
        const bool hasMatSpec = materialPatchSpec && *materialPatchSpec;
        const bool hasGeoFilter = geoIds && geoCount > 0;
        if (!hasTexPath && !hasMatSpec && !hasGeoFilter) return false;

        // Normalise to the same lowercase/.m2 form the host uses for real file I/O -- this is
        // still what gets read off disk below -- then mangle in itemDisplayId to get the actual
        // table key. Two different displayIds that resolve to the same underlying model file each
        // get their own entry instead of colliding on one shared key. VirtualProvide does not do
        // any un-mangling on the read side; it just normalises the incoming request the same way
        // and looks it up verbatim, so whatever requests this path must ask for the virtual form,
        // not the real one -- see the doc comment on this function in VirtualPath.hpp.
        char normPath[264];
        NormalizeRealPath(normPath, sizeof(normPath), realMdxPath);

        char vKey[280];
        BuildGlobalVirtualKey(vKey, sizeof(vKey), normPath, itemDisplayId);

        if (outVirtualPath && outVirtualPathSz)
        {
            size_t n = std::strlen(vKey);
            if (n >= outVirtualPathSz) n = outVirtualPathSz - 1;
            std::memcpy(outVirtualPath, vKey, n);
            outVirtualPath[n] = '\0';
        }

        // First (texPath, materialPatchSpec) registered for this (path, itemDisplayId) pair wins
        // for the rest of the session -- see the doc comment on this function in VirtualPath.hpp.
        if (g_globalOverrides.count(vKey)) return true;

        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(normPath, mdxBytes))
        {
            VPathLog("  VPathPopulateGlobal: mdx READ FAILED '%s'", normPath);
            return false;
        }

        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), normPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent for some models; that is OK

        // Same ApplySkinByteFilter used by VPathPopulate -- zeroes the raw index bytes of every
        // submesh whose sectionId isn't in geoIds, in place, before the bytes go in the table. A
        // no-op (returns immediately) when geoCount == 0 or skinBytes is empty/malformed.
        if (hasGeoFilter)
            ApplySkinByteFilter(skinBytes, geoIds, geoCount);

        // Material-CSV texture-value patching runs BEFORE the base ModelTexture_N bake -- see the
        // matching comment in VPathPopulate above. A sidecar row claims and HARDCODEs its own
        // texture-unit records first, whatever TextureType it declares; PatchReplaceableTextureTypes
        // then only promotes whatever OBJECT_SKIN(2) records are still left -- its own single fixed
        // type, unrelated to the sidecar -- so a sidecar row that happens to target TextureType=2
        // has its CSV value win instead of being silently pre-empted by texPath. WEAPON_BLADE(3) is
        // never touched here at all; only an explicit sidecar TextureType=3 row can ever set it.
        // This only ever touches modelBytes, not skinBytes, so it no longer needs a skin file.
        if (hasMatSpec)
            PatchTargetedMaterialTextures(mdxBytes, materialPatchSpec);

        if (hasTexPath)
        {
            PatchReplaceableTextureTypes(mdxBytes, texPath);
        }

        VPathLog("  VPathPopulateGlobal: '%s' (displayId=%u) -> vkey='%s' mdx=%zu skin=%zu bytes "
                 "(tex='%s' spec='%s' geoCount=%u)",
                 normPath, itemDisplayId, vKey, mdxBytes.size(), skinBytes.size(),
                 hasTexPath ? texPath : "", hasMatSpec ? materialPatchSpec : "", geoCount);

        char vSkin[280];
        VirtualSkinPath(vSkin, sizeof(vSkin), vKey);

        const bool hasSkin = !skinBytes.empty();
        g_globalOverrides.emplace(vKey, std::move(mdxBytes));
        if (hasSkin)
            g_globalOverrides.emplace(vSkin, std::move(skinBytes));

        if (evictable)
        {
            const std::string pool = (evictionPool && *evictionPool) ? evictionPool : "Creature";
            RegisterEvictableGroup(pool, vKey, vSkin, hasSkin);
        }

        return true;
    }

    void VPathRegisterLazyResolver(VPathLazyResolver resolver)
    {
        std::lock_guard<std::recursive_mutex> vpLock(g_vpMutex);
        if (resolver) g_lazyResolvers.push_back(resolver);
    }

    bool VPathDecodeGlobalVirtualKey(const char* virtualPathName, char* outNormPath, size_t outNormPathSz,
                                     uint32_t* outDisplayId)
    {
        if (!virtualPathName || !outNormPath || outNormPathSz == 0 || !outDisplayId) return false;

        const char* lastDot = nullptr;
        for (const char* p = virtualPathName; *p; ++p) if (*p == '.') lastDot = p;
        if (!lastDot) return false; // no extension -- not a shape BuildGlobalVirtualKey produces

        // Walk backward from the extension over ASCII digits.
        const char* digitsEnd = lastDot;
        const char* digitsBegin = digitsEnd;
        while (digitsBegin > virtualPathName && digitsBegin[-1] >= '0' && digitsBegin[-1] <= '9')
            --digitsBegin;
        if (digitsBegin == digitsEnd) return false;                       // no digit run at all
        if (digitsBegin == virtualPathName || digitsBegin[-1] != '_') return false; // no '_' before it

        uint32_t displayId = 0;
        for (const char* d = digitsBegin; d < digitsEnd; ++d)
            displayId = displayId * 10u + static_cast<uint32_t>(*d - '0');

        const size_t stemLen = static_cast<size_t>((digitsBegin - 1) - virtualPathName); // exclude '_'
        const size_t extLen  = std::strlen(lastDot);
        if (stemLen + extLen >= outNormPathSz) return false;

        std::memcpy(outNormPath, virtualPathName, stemLen);
        std::memcpy(outNormPath + stemLen, lastDot, extLen + 1); // includes null terminator
        *outDisplayId = displayId;
        return true;
    }

    namespace
    {
        // Resolves the HMODULE of whichever module this very function's own code lives in -- i.e.
        // WarcraftXL.dll itself (this module is built into it, there is no separate
        // WXLExtendedEquipment.dll) -- without needing DllMain's hinstDLL (not tracked anywhere in
        // this module) and without assuming the process's current working directory has any
        // relationship to the DLL's own location. GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: this
        // is a lookup, not a LoadLibrary -- don't bump the module's refcount for it. Shared by
        // WxlIniGetBool and WxlIniGetInt so the resolution logic only exists once.
        bool ResolveIniPath(char* outIniPath, size_t outIniPathSz)
        {
            // In-tree there is no WarcraftXL.dll: the ini sits beside the client executable.
            const char* kIniName = "WXLExtendedEquipment.ini";
#ifdef _WIN32
            char exePath[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
            if (n == 0 || n >= sizeof(exePath)) return false;

            const char* lastSlash = std::strrchr(exePath, '\\');
            if (lastSlash)
            {
                const size_t dirLen = static_cast<size_t>(lastSlash - exePath) + 1; // include the '\\'
                if (dirLen + std::strlen(kIniName) >= outIniPathSz) return false;
                std::memcpy(outIniPath, exePath, dirLen);
                std::strcpy(outIniPath + dirLen, kIniName);
                return true;
            }
#endif
            if (std::strlen(kIniName) >= outIniPathSz) return false;
            std::strcpy(outIniPath, kIniName); // no directory component: relative to the working dir
            return true;
        }
    }

    int WxlIniGetInt(const char* section, const char* key, int defaultValue)
    {
        if (!section || !key) return defaultValue;

        char iniPath[260] = {};
        if (!ResolveIniPath(iniPath, sizeof(iniPath))) return defaultValue;

#ifdef _WIN32
        // GetPrivateProfileInt's convention: a missing file, section or key returns the default.
        return static_cast<int>(GetPrivateProfileIntA(section, key, defaultValue, iniPath));
#else
        (void)iniPath;
        return defaultValue;
#endif
    }

    bool WxlIniGetBool(const char* section, const char* key, bool defaultValue)
    {
        return WxlIniGetInt(section, key, defaultValue ? 1 : 0) != 0;
    }
}
