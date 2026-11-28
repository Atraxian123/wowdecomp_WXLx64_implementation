// WXL: shader 23 WMO materials. See MapObjShader23.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)

#include "WoW/Source/WorldClient/MapObjShader23.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/WXL/Ini.h"
#include "Engine/Source/WXL/Log.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/Shader23Format.h"

namespace wxl::shader23
{
    constexpr uint32_t kMaxGroupRecords = 512;
    constexpr uint32_t kMaxMaterialIndex = 0x10000;
    constexpr uint32_t kMaxGroupNumber = 0x10000;

    // Everything computed for one group record, built the first time a batch of that group is drawn.
    struct GroupData
    {
        uint32_t first = 0;               // first stored vertex
        uint32_t count = 0;               // stored vertices
        std::vector<C2Vector> uv[S23_MAX_UV_SETS]; // indexed by absolute vertex number (the head is zero)
        const uint8_t* weights = nullptr; // count * 4 bytes inside the file image, or null
        std::vector<CImVector> colors[16][S23_LAYERS]; // [layer mask][layer], built when first needed

        // Outdoor lighting of the whole stored range, rebuilt only when the sun / ambient / diffuse (or the group's
        // orientation) changed: litKey holds the quantised light direction in the group's space and the 6 colour bytes.
        std::vector<CImVector> lit;
        int32_t litKey[9] = {};
        bool litValid = false;
    };

    // One root WMO's sidecar. The file is read by the async file queue; the root is only used once `ready`.
    struct Root
    {
        const CMapObj* owner = nullptr;
        uint8_t* file = nullptr;          // SMemAlloc'd, filled by the async read
        size_t fileSize = 0;
        CAsyncObject* async = nullptr;    // pending read, null once it finished or was cancelled
        bool ready = false;
        const S23Header* hdr = nullptr;
        const S23Material* mats = nullptr;
        const S23Group* groups = nullptr;
        const char* strings = nullptr;
        std::vector<int32_t> matRecord;   // MOMT index -> material record, -1 when not a shader 23 material
        std::vector<int32_t> groupRecord; // group number -> group record, -1 when the sidecar has none
        const CMapObjGroup* cacheGroup = nullptr; // batches of one group are drawn back to back: remember its record
        int32_t cacheRecord = -1;
        std::vector<CTexture*> textures;                     // [record * S23_LAYERS + layer]
        std::vector<uint8_t> textureTried;
        std::vector<std::unique_ptr<GroupData>> groupData;   // [record]

        Root() = default;
        Root(const Root&) = delete;
        Root& operator=(const Root&) = delete;

        ~Root()
        {
            if (file) {
                SMemFree(file, __FILE__, __LINE__, 0);
            }
        }
    };

    namespace
    {
        std::unordered_map<const CMapObj*, std::unique_ptr<Root>> s_roots;

        bool Enabled()
        {
            return wxl::ini::GetBool("WMO", "Shader23", true);
        }

        bool Validate(Root& r)
        {
            const uint8_t* base = r.file;
            const size_t size = r.fileSize;
            const S23Header* h = reinterpret_cast<const S23Header*>(base);

            if (h->magic != S23_MAGIC || h->version != S23_VERSION) {
                return false;
            }

            const uint64_t matEnd = static_cast<uint64_t>(h->materialsOffset) + static_cast<uint64_t>(h->materialCount) * sizeof(S23Material);
            const uint64_t grpEnd = static_cast<uint64_t>(h->groupsOffset) + static_cast<uint64_t>(h->groupCount) * sizeof(S23Group);
            const uint64_t strEnd = static_cast<uint64_t>(h->stringsOffset) + h->stringsSize;

            if (matEnd > size || grpEnd > size || strEnd > size || h->groupCount > kMaxGroupRecords) {
                return false;
            }

            r.hdr = h;
            r.mats = reinterpret_cast<const S23Material*>(base + h->materialsOffset);
            r.groups = reinterpret_cast<const S23Group*>(base + h->groupsOffset);
            r.strings = reinterpret_cast<const char*>(base + h->stringsOffset);

            uint32_t matSlots = 0;
            uint32_t groupSlots = 0;

            for (uint32_t i = 0; i < h->materialCount; i++) {
                if (r.mats[i].materialIndex < kMaxMaterialIndex) {
                    matSlots = std::max(matSlots, r.mats[i].materialIndex + 1);
                }
            }

            r.matRecord.assign(matSlots, -1);

            for (uint32_t i = 0; i < h->materialCount; i++) {
                if (r.mats[i].materialIndex < matSlots) {
                    r.matRecord[r.mats[i].materialIndex] = static_cast<int32_t>(i);
                }
            }

            for (uint32_t i = 0; i < h->groupCount; i++) {
                const S23Group& g = r.groups[i];
                const uint64_t bytes = static_cast<uint64_t>(g.vertexCount) * (g.uvSetCount * 8u + ((g.flags & S23_GROUP_HAS_WEIGHTS) ? 4u : 0u));

                if (g.uvSetCount < 1 || g.uvSetCount > S23_MAX_UV_SETS || static_cast<uint64_t>(g.dataOffset) + bytes > size || g.firstVertex + g.vertexCount > 0xFFFFu + 1u) {
                    return false;
                }

                if (g.groupIndex < kMaxGroupNumber) {
                    groupSlots = std::max(groupSlots, g.groupIndex + 1);
                }
            }

            r.groupRecord.assign(groupSlots, -1);

            for (uint32_t i = 0; i < h->groupCount; i++) {
                if (r.groups[i].groupIndex < groupSlots) {
                    r.groupRecord[r.groups[i].groupIndex] = static_cast<int32_t>(i);
                }
            }

            r.textures.assign(static_cast<size_t>(h->materialCount) * S23_LAYERS, nullptr);
            r.textureTried.assign(r.textures.size(), 0);
            r.groupData.resize(h->groupCount);
            return true;
        }

        // The group's number in its root (the NNN of the group file), or -1.
        int32_t GroupNumber(const CMapObj* mapObj, const CMapObjGroup* group)
        {
            for (int32_t i = 0; i < mapObj->m_mapObjGroupCount; i++) {
                if (mapObj->m_mapObjGroupArray[i] == group) {
                    return i;
                }
            }

            return -1;
        }

        // The layer's texture once its file is requested; null while the sidecar names none or it did not load.
        CTexture* LayerTexture(Root& r, uint32_t record, uint32_t layer)
        {
            const size_t slot = static_cast<size_t>(record) * S23_LAYERS + layer;

            if (!r.textureTried[slot]) {
                r.textureTried[slot] = 1;
                const S23Texture& t = r.mats[record].layer[layer];

                if (t.fileDataId && t.pathOffset != S23_NO_STRING && t.pathOffset < r.hdr->stringsSize) {
                    std::string path(r.strings + t.pathOffset);

                    for (char& c : path) {
                        if (c == '/') {
                            c = '\\';
                        }
                    }

                    r.textures[slot] = CMap::LoadTexture(path.c_str());
                }
            }

            return r.textures[slot];
        }

        GroupData& Group(Root& r, uint32_t record)
        {
            std::unique_ptr<GroupData>& slot = r.groupData[record];

            if (slot) {
                return *slot;
            }

            const S23Group& g = r.groups[record];
            slot = std::make_unique<GroupData>();
            slot->first = g.firstVertex;
            slot->count = g.vertexCount;

            const uint8_t* block = r.file + g.dataOffset;
            const size_t total = static_cast<size_t>(g.firstVertex) + g.vertexCount;

            for (uint32_t s = 0; s < S23_MAX_UV_SETS; s++) {
                slot->uv[s].assign(total, C2Vector());
                const uint32_t src = s < g.uvSetCount ? s : g.uvSetCount - 1; // a missing set repeats the last stored one
                std::memcpy(&slot->uv[s][g.firstVertex], block + static_cast<size_t>(src) * g.vertexCount * 8u, static_cast<size_t>(g.vertexCount) * 8u);
            }

            if (g.flags & S23_GROUP_HAS_WEIGHTS) {
                slot->weights = block + static_cast<size_t>(g.uvSetCount) * g.vertexCount * 8u;
            }

            return *slot;
        }

        // Vertex colours for layer `k` when the material has the layers in `mask`: the group's baked colour with the
        // blend alpha. The first present layer is drawn opaque (its alpha is never read); a later layer gets
        // weight[k] / (sum of the weights of the present layers up to k).
        const std::vector<CImVector>& LayerColors(GroupData& gd, const CMapObjGroup* group, uint32_t mask, uint32_t k)
        {
            std::vector<CImVector>& out = gd.colors[mask & 15][k];

            if (!out.empty()) {
                return out;
            }

            const size_t total = static_cast<size_t>(gd.first) + gd.count;
            out.assign(total, CImVector());

            CImVector base;
            base.r = base.g = base.b = (group->m_parent->m_header->m_flags & 0x2) ? 0 : 0x7F;
            base.a = 0xFF;

            for (uint32_t i = 0; i < gd.count; i++) {
                const uint32_t v = gd.first + i;
                CImVector c = group->m_colorVertexList ? group->m_colorVertexList[v] : base;
                uint32_t w[S23_LAYERS] = { 255, 0, 0, 0 };

                if (gd.weights) {
                    for (uint32_t j = 0; j < S23_LAYERS; j++) {
                        w[j] = gd.weights[i * 4 + j];
                    }
                }

                uint32_t sum = 0;
                bool first = true;

                for (uint32_t j = 0; j <= k; j++) {
                    if (mask & (1u << j)) {
                        sum += w[j];
                        first = first && j == k;
                    }
                }

                c.a = first ? 0xFF : static_cast<uint8_t>(sum ? (w[k] * 255u + sum / 2) / sum : 0);
                out[v] = c;
            }

            return out;
        }
    }

    namespace
    {
        // Per-vertex day/night lighting of an outdoor group, computed on the CPU with the formula the stock fixed-function
        // WMO path gets from the device: the baked vertex colour is the emissive term, the material is 0x7F7F7F for both
        // ambient and diffuse, and light 0 carries the day/night ambient and sun of "light 1" (the same values
        // CMapObjGroup::SetLighting mode 1 hands to the stock draw):
        //     colour = saturate(vertexColour + 0x7F/255 * (ambient + sunDiffuse * max(0, N . toSun)))
        // It is done here instead of by the device because device lighting replaces the vertex alpha with the material
        // alpha, and the vertex alpha carries the layer weights.
        // The sun direction is a world-space direction; the vertex normals are in the group's space, so it is brought
        // into that space with the current world matrix (a rotation, possibly with a uniform scale).
        //
        // The result only depends on the sun direction in the group's space and on the ambient/diffuse bytes, so the whole
        // stored range is lit once and reused until one of those changes (the direction is compared at 1/1024 steps, well
        // under one colour level). Before, every batch re-lit its vertices every frame.
        void LightGroup(const CMapObjGroup* group, GroupData& gd, const std::vector<CImVector>& baked)
        {
            DayNight::DNInfo* info = DayNight::GetInfo();
            const float k = 0.0039215689f;
            const float m = 127.0f * k;
            const C3Vector& sun = CMap::s_mapLight->m_light.m_dir; // the direction the sunlight travels
            const CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_World];
            const C44Matrix& w = stack.m_mtx[stack.m_level];
            const bool identity = (stack.m_flags[stack.m_level] & CGxMatrixStack::F_Identity) != 0;

            // toSun in the group's space: (row-vector matrices) n_world = n * M3, so n_world . L = n . (M3 * L)
            float lx = -sun.x, ly = -sun.y, lz = -sun.z;

            if (!identity) {
                const float ox = w.a0 * lx + w.a1 * ly + w.a2 * lz;
                const float oy = w.b0 * lx + w.b1 * ly + w.b2 * lz;
                const float oz = w.c0 * lx + w.c1 * ly + w.c2 * lz;
                lx = ox;
                ly = oy;
                lz = oz;
            }

            const float len = std::sqrt(lx * lx + ly * ly + lz * lz);

            if (len > 0.0f) {
                lx /= len;
                ly /= len;
                lz /= len;
            }

            const int32_t key[9] = {
                static_cast<int32_t>(std::lround(lx * 1024.0f)), static_cast<int32_t>(std::lround(ly * 1024.0f)), static_cast<int32_t>(std::lround(lz * 1024.0f)),
                info->m_light1.m_ambient.r, info->m_light1.m_ambient.g, info->m_light1.m_ambient.b,
                info->m_light1.m_diffuse.r, info->m_light1.m_diffuse.g, info->m_light1.m_diffuse.b
            };

            const size_t total = static_cast<size_t>(gd.first) + gd.count;

            if (gd.litValid && gd.lit.size() >= total && std::memcmp(key, gd.litKey, sizeof(key)) == 0) {
                return;
            }

            std::memcpy(gd.litKey, key, sizeof(key));
            gd.litValid = true;

            if (gd.lit.size() < total) {
                gd.lit.resize(total);
            }

            const float amb[3] = { info->m_light1.m_ambient.r * k, info->m_light1.m_ambient.g * k, info->m_light1.m_ambient.b * k };
            const float dif[3] = { info->m_light1.m_diffuse.r * k, info->m_light1.m_diffuse.g * k, info->m_light1.m_diffuse.b * k };

            for (uint32_t v = gd.first; v < total; v++) {
                const C3Vector& n = group->m_normalList[v];
                const float ndl = len > 0.0f ? std::max(0.0f, n.x * lx + n.y * ly + n.z * lz) : 0.0f;
                const CImVector& c = baked[v];
                const float in[3] = { c.r * k, c.g * k, c.b * k };
                float out[3];

                for (int i = 0; i < 3; i++) {
                    out[i] = std::min(1.0f, in[i] + m * (amb[i] + dif[i] * ndl));
                }

                gd.lit[v].r = static_cast<uint8_t>(out[0] * 255.0f + 0.5f);
                gd.lit[v].g = static_cast<uint8_t>(out[1] * 255.0f + 0.5f);
                gd.lit[v].b = static_cast<uint8_t>(out[2] * 255.0f + 0.5f);
                gd.lit[v].a = 0xFF; // the layer alpha is written per pass
            }
        }

        void AsyncFailedCallback(CAsyncObject* object)
        {
            void* buffer = object->m_buffer;
            AsyncFileReadDestroyObject(object);
            SMemFree(buffer, __FILE__, __LINE__, 0);
        }

        // Runs on the main thread from the async file poll handler once the read finished.
        void PostloadCallback(void* arg)
        {
            Root* r = static_cast<Root*>(arg);
            AsyncFileReadDestroyObject(r->async);
            r->async = nullptr;

            if (Validate(*r)) {
                r->ready = true;
                wxl::Log("[Shader23] sidecar loaded: %u material(s), %u group(s)", r->hdr->materialCount, r->hdr->groupCount);
            } else {
                wxl::Log("[Shader23] not a usable version %u sidecar, ignored", S23_VERSION);
                SMemFree(r->file, __FILE__, __LINE__, 0);
                r->file = nullptr;
            }
        }
    }

    void Load(CMapObj* mapObj, const char* wmoName)
    {
        Unload(mapObj);

        if (!Enabled() || !wmoName) {
            return;
        }

        std::string path(wmoName);
        const size_t dot = path.find_last_of('.');

        if (dot == std::string::npos) {
            return;
        }

        path.replace(dot, std::string::npos, ".s23");

        SFileImpl* file = nullptr;

        if (!SFileOpenFile(path.c_str(), &file) || !file) {
            return; // no sidecar: an ordinary WMO
        }

        const uint32_t size = SFileGetFileSize(file, nullptr);

        if (size < sizeof(S23Header) || size >= (1u << 30)) {
            SFileCloseFile(file);
            return;
        }

        // The read goes through the async file queue like the root WMO's own (same priority, queued right behind it), and
        // the sidecar is only used once it is validated in the postload callback on the main thread. Until then
        // HasData() is false and the stock draw runs.
        auto root = std::make_unique<Root>();
        root->owner = mapObj;
        root->fileSize = size;
        root->file = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));

        CAsyncObject* object = AsyncFileReadAllocObject();
        object->m_file = file;
        object->m_buffer = root->file;
        object->m_size = size;
        object->m_userArg = root.get();
        object->m_userPostloadCallback = reinterpret_cast<void*>(PostloadCallback);
        object->m_priority = 124;
        root->async = object;

        s_roots[mapObj] = std::move(root);
        AsyncFileReadObject(object, 0);
    }

    void Unload(CMapObj* mapObj)
    {
        const auto it = s_roots.find(mapObj);

        if (it == s_roots.end()) {
            return;
        }

        Root& r = *it->second;

        if (r.async) {
            // same protocol as CMapObj::Clear: when the read is in flight the cancel callback frees the buffer later
            const int32_t freeData = AsyncFile::Cancel(r.async, AsyncFailedCallback);
            r.async = nullptr;

            if (!freeData) {
                r.file = nullptr;
            }
        }

        for (CTexture*& t : r.textures) {
            if (t) {
                HandleClose(reinterpret_cast<HOBJECT>(t));
                t = nullptr;
            }
        }

        s_roots.erase(it);
    }

    Root* Find(const CMapObj* mapObj)
    {
        if (s_roots.empty()) {
            return nullptr;
        }

        const auto it = s_roots.find(mapObj);
        return it != s_roots.end() && it->second->ready ? it->second.get() : nullptr;
    }

    bool HasData(const CMapObj* mapObj)
    {
        return Find(mapObj) != nullptr;
    }

    bool DrawBatch(Root* root, CMapObj* mapObj, CMapObjGroup* group, const SMOBatch& batch, const SMOMaterial& material, int32_t fogMode)
    {
        if (!root) {
            return false;
        }

        // a material with a blend mode of its own (additive, alpha, ...) keeps the stock draw: the layer blend is opaque
        if (material.m_blendMode != 0) {
            return false;
        }

        Root& r = *root;
        const int32_t matRecord = batch.m_texture < r.matRecord.size() ? r.matRecord[batch.m_texture] : -1;

        if (matRecord < 0) {
            return false;
        }

        const uint32_t mask = r.mats[matRecord].layerMask & 15u;

        if (!mask) {
            return false;
        }

        if (r.cacheGroup != group) {
            const int32_t groupNumber = GroupNumber(mapObj, group);
            r.cacheGroup = group;
            r.cacheRecord = groupNumber >= 0 && static_cast<size_t>(groupNumber) < r.groupRecord.size() ? r.groupRecord[groupNumber] : -1;
        }

        const int32_t grpRecord = r.cacheRecord;

        if (grpRecord < 0) {
            return false;
        }

        // the batch's vertices must all be inside the stored range, and the group's own arrays must cover the stored range
        // (the per-vertex colour / normal arrays are read over all of it)
        const S23Group& stored = r.groups[grpRecord];

        if (!stored.vertexCount || batch.m_vertexStart < stored.firstVertex || static_cast<uint32_t>(batch.m_vertexEnd) >= stored.firstVertex + stored.vertexCount
            || static_cast<int32_t>(stored.firstVertex + stored.vertexCount) > group->m_vertexListCount) {
            return false;
        }

        GroupData& gd = Group(r, static_cast<uint32_t>(grpRecord));

        // every layer texture has to be ready, so a layer never shows up half-way through streaming in
        CGxTex* gxTex[S23_LAYERS] = {};
        bool ready = true;

        for (uint32_t k = 0; k < S23_LAYERS; k++) {
            if (!(mask & (1u << k))) {
                continue;
            }

            CTexture* t = LayerTexture(r, static_cast<uint32_t>(matRecord), k);
            gxTex[k] = t ? TextureGetGxTex(t, 0, nullptr) : nullptr;
            ready = ready && gxTex[k] != nullptr;
        }

        if (!ready) {
            return false;
        }

        GxRsSet(GxRs_Culling, static_cast<int32_t>((material.m_flags & 0x4) == 0));

        g_theGxDevicePtr->RsPush();

        // The stock calls feed shader constants; this draw runs in the fixed-function pipeline.
        g_theGxDevicePtr->RsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
        g_theGxDevicePtr->RsSet(GxRs_Lighting, 0);
        g_theGxDevicePtr->RsSet(GxRs_AlphaRef, 0);

        // Outdoor groups (0x8 exterior, 0x40 exterior lit) are drawn the way the stock outdoor path draws them: lit by
        // the day/night sun and ambient (see LightGroup), always with the exterior fog and ignoring the "unfogged"
        // flag. A material with the unlit flag (0x1) and every indoor group keep the baked colours only.
        const bool outdoor = (group->m_flags & 0x48) != 0;
        const bool lit = outdoor && !(material.m_flags & 0x1) && group->m_normalList && static_cast<int32_t>(gd.first + gd.count) <= group->m_normalListCount;

        if (outdoor) {
            fogMode = 2;
        }

        int32_t fogOn = 0;

        if ((outdoor || !(material.m_flags & 0x2)) && g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog)) {
            DayNight::DNInfo* info = DayNight::GetInfo();
            DNFogInfo* fog = (fogMode & 0x2) ? &info->m_fog : &info->m_fogInterior;
            g_theGxDevicePtr->RsSet(GxRs_FogStart, fog->m_start);
            g_theGxDevicePtr->RsSet(GxRs_FogEnd, fog->m_end);
            g_theGxDevicePtr->RsSet(GxRs_FogColor, *reinterpret_cast<const int32_t*>(&fog->m_color));
            fogOn = 1;
        }

        g_theGxDevicePtr->RsSet(GxRs_Fog, fogOn);

        // Stage 0 only: layer texture * vertex colour * 2 (the baked colours are centred on 0x7F), alpha from the vertices.
        g_theGxDevicePtr->RsSet(GxRs_ColorOp0, static_cast<int32_t>(GxTexOp_Mod2x));
        g_theGxDevicePtr->RsSet(GxRs_AlphaOp0, static_cast<int32_t>(GxTexOp_PassThru));
        g_theGxDevicePtr->RsSet(GxRs_TextureShader0, 0);
        g_theGxDevicePtr->RsSet(GxRs_TexGen0, 0);
        g_theGxDevicePtr->RsSet(GxRs_TextureCoord0, 0);
        g_theGxDevicePtr->RsSet(GxRs_Texture1, static_cast<void*>(nullptr));
        g_theGxDevicePtr->RsSet(GxRs_Texture2, static_cast<void*>(nullptr));
        g_theGxDevicePtr->RsSet(GxRs_Texture3, static_cast<void*>(nullptr));

        CGxBatch draw;
        draw.m_primType = GxPrim_Triangles;
        draw.m_start = batch.m_indexStart;
        draw.m_count = batch.m_indexCount;
        draw.m_minIndex = batch.m_vertexStart;
        draw.m_maxIndex = batch.m_vertexEnd;

        // Only the vertices this batch references are streamed (indices stay absolute: the stream is sized for
        // vertexEnd + 1 but only [vertexStart, vertexEnd] is written).
        const uint32_t firstVertex = batch.m_vertexStart;
        const uint32_t rangeCount = static_cast<uint32_t>(batch.m_vertexEnd) - firstVertex + 1;
        bool firstLayer = true;

        // outdoor: the group's lit colours (rebuilt only when the light changed); only the alpha differs per layer
        if (lit) {
            LightGroup(group, gd, LayerColors(gd, group, mask, 0)); // the group's baked colours (alpha unused)
        }

        for (uint32_t k = 0; k < S23_LAYERS; k++) {
            if (!(mask & (1u << k))) {
                continue;
            }

            const std::vector<CImVector>* colors = &LayerColors(gd, group, mask, k);

            if (lit) {
                // each draw streams its vertices right away, so the layer's alpha can be written over the shared array
                std::vector<CImVector>& litColors = gd.lit;

                for (uint32_t v = firstVertex; v < firstVertex + rangeCount; v++) {
                    litColors[v].a = (*colors)[v].a;
                }

                colors = &litColors;
            }

            GxTexSetWrap(gxTex[k], GxTex_Wrap, GxTex_Wrap); // the layers tile
            g_theGxDevicePtr->RsSet(GxRs_Texture0, gxTex[k]);
            g_theGxDevicePtr->RsSet(GxRs_BlendingMode, static_cast<int32_t>(firstLayer ? GxBlend_Opaque : GxBlend_Alpha));
            GxPrimVertexPtrRange(firstVertex, rangeCount, group->m_vertexList, sizeof(C3Vector), group->m_normalList, sizeof(C3Vector), colors->data(), sizeof(CImVector),
                                 gd.uv[k].data(), sizeof(C2Vector), nullptr, 0);
            g_theGxDevicePtr->Draw(&draw, 1);
            firstLayer = false;
        }

        g_theGxDevicePtr->RsPop();

        // the rest of the group's batches read the group's own vertex stream
        group->SetVertexVB();
        return true;
    }
}
