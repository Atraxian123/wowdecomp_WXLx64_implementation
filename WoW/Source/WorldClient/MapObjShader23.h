// WXL: shader 23 WMO materials, drawn from the "<root name>.s23" sidecar that wxl-converter writes beside a root WMO.
// Format: Shader23Format.h (the converter's own header, unchanged).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// A shader 23 material blends up to four tiling layers by per-vertex weights. The 3.3.5a material record and group
// hold two textures, two UV sets and no weights, so the sidecar carries the rest: the layer textures, UV sets 0..3 and
// the four weights of every vertex the shader 23 batches use.
//
// What is drawn: a batch whose material index is listed in the sidecar is drawn once per layer, in the fixed-function
// pipeline: layer k with its own texture and UV set k, vertex colour * 2 for the baked lighting, fog from the day/night
// fog. The first layer replaces, every later layer is alpha-blended over it with
//     weight[k] / (weight[0] + ... + weight[k])
// as the per-vertex alpha, which makes the frame hold  sum(weight[k] * layer[k])  after the last pass.
// Without a sidecar nothing changes: every call below returns "not handled" and the stock draw runs.
//
// [WMO] Shader23=0 in WXLSetup.ini turns it off (default 1).
#pragma once

#include <cstdint>

class CMapObj;
class CMapObjGroup;
struct SMOBatch;
struct SMOMaterial;

namespace wxl::shader23
{
    struct Root;

    /// Looks for "<root>.s23" beside the root WMO and queues its read on the async file queue (same priority as the root
    /// WMO's own read). Call once the root's name is final. The sidecar is used once the read is done and validated.
    void Load(CMapObj* mapObj, const char* wmoName);

    /// Frees everything Load made for this root (textures, arrays) and cancels a read still in flight. Safe when nothing was loaded.
    void Unload(CMapObj* mapObj);

    /// The root's sidecar when it is loaded and usable, else null (no sidecar, still reading, or rejected). One hash
    /// lookup: callers fetch it once per group and hand it to DrawBatch for every batch.
    Root* Find(const CMapObj* mapObj);

    /// True when this root has a usable sidecar.
    bool HasData(const CMapObj* mapObj);

    /// Draws `batch` as a shader 23 blend when its material is in the sidecar and everything it needs is ready.
    /// `root` comes from Find() (null returns false). Returns false (nothing drawn, no state changed) when the caller has
    /// to draw the batch the stock way. `fogMode` is the one the stock loop uses (2 exterior fog, 1 interior fog).
    bool DrawBatch(Root* root, CMapObj* mapObj, CMapObjGroup* group, const SMOBatch& batch, const SMOMaterial& material, int32_t fogMode);
}
