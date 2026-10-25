// Guarded reads of the engine's model instances. See ModelAccess.h.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// What this does is what the in-tree physics glue did before it became a module: copy an instance's inputs out
// of client memory in guarded steps, report the step that failed, and write a bone palette back the same way.
//   * bone world = palette bone x the scene matrix (CM2Scene::m_viewInv, a MIRROR: handed over as 16 floats);
//   * the placement is the instance's own matrix (CM2Model::m_matrixF4);
//   * the clock is the scene's (CM2Scene::m_time, ms).

#include "Engine/Source/WXLApi/ModelAccess.h"

#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"

#include <windows.h>

#include <cstring>

namespace wxl::api::model
{
    static_assert(sizeof(C44Matrix) == 64, "the bone palette and the matrices are 16 floats");

    namespace
    {
        // No C++ objects in a function with __try, so every guarded copy is its own tiny function.
        bool SafeCopy(void* dst, const void* src, size_t n) noexcept
        {
            __try { std::memcpy(dst, src, n); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        struct Resolved
        {
            const CM2Model*   model = nullptr;
            CM2Shared*        shared = nullptr;
            const CM2Scene*   scene = nullptr;
            const C44Matrix*  palette = nullptr;    // the instance's live palette
            const M2CompBone* bones = nullptr;      // the model's bone records
            uint32_t          boneCount = 0;
            char              path[WXL_PATH_MAX] = {};
        };

        // Stops at the first step that does not hold and returns its WXL_READ_* code.
        int Resolve(void* instance, Resolved& r) noexcept
        {
            r.model = static_cast<const CM2Model*>(instance);
            if (!r.model) return WXL_READ_NO_MODEL;
            const M2Data* hdr = nullptr;
            uint32_t sharedFlags = 0, instFlags = 0;

            if (!SafeCopy(&r.shared, &r.model->m_shared, sizeof(r.shared)) || !r.shared) return WXL_READ_NO_MODEL;
            if (!SafeCopy(r.path, r.shared->m_fileName, sizeof(r.path) - 1)) { r.path[0] = 0; return WXL_READ_NO_PATH; }
            r.path[sizeof(r.path) - 1] = 0;
            // The model must be fully loaded AND parsed: before that bit is set the header's arrays are not
            // relocated, and Data() points at garbage.
            if (!SafeCopy(&sharedFlags, &r.shared->m_flags, 4) || !(sharedFlags & 1u)) return WXL_READ_NOT_PARSED;
            if (!SafeCopy(&hdr, &r.shared->m_data, sizeof(hdr)) || !hdr) return WXL_READ_NO_HEADER;
            if (!SafeCopy(&r.boneCount, &hdr->m_bones.m_count, 4)) return WXL_READ_NO_BONE_COUNT;
            if (r.boneCount < 2 || r.boneCount > WXL_MAX_BONES) return WXL_READ_BONE_COUNT;   // one bone: nothing can swing
            if (!SafeCopy(&instFlags, &r.model->m_fFlags, 4) || !(instFlags & 1u)) return WXL_READ_NOT_ANIMATED;
            if (!SafeCopy(&r.scene, &r.model->m_scene, sizeof(r.scene)) || !r.scene) return WXL_READ_NO_SCENE;
            if (!SafeCopy(&r.palette, &r.model->m_boneMatrices, sizeof(r.palette)) || !r.palette) return WXL_READ_NO_PALETTE;
            r.bones = hdr->m_bones.Data();
            if (!r.bones) return WXL_READ_NO_BONES;
            return WXL_READ_OK;
        }
    }

    int ReadInstance(void* instance, WxlInstanceView* out)
    {
        if (!out || out->structSize < sizeof(WxlInstanceView)) return WXL_READ_NO_MODEL;
        Resolved r;
        const int stage = Resolve(instance, r);
        // the path and the model id are worth having even when a later step failed (the module logs them)
        std::memcpy(out->path, r.path, sizeof(out->path));
        out->model = r.shared;
        out->boneCount = r.boneCount;
        if (stage) return stage;

        if (!SafeCopy(&out->clockMs, &r.scene->m_time, 4)) return WXL_READ_NO_CLOCK;
        if (!SafeCopy(out->sceneMatrix, &r.scene->m_viewInv, sizeof(out->sceneMatrix))) return WXL_READ_NO_SCENE_MAT;
        if (!SafeCopy(out->ownMatrix, &r.model->m_matrixF4, sizeof(out->ownMatrix))) return WXL_READ_NO_OWN_MAT;
        return WXL_READ_OK;
    }

    int ReadBones(void* instance, uint32_t count, float* palette, int16_t* parents, float* pivots)
    {
        Resolved r;
        const int stage = Resolve(instance, r);
        if (stage) return stage;
        if (count != r.boneCount) return WXL_READ_BONE_COUNT;

        if (palette && !SafeCopy(palette, r.palette, size_t(count) * 64)) return WXL_READ_NO_PALETTE;
        if (parents || pivots)
        {
            for (uint32_t k = 0; k < count; ++k)
            {
                uint16_t parent = 0xFFFF;
                C3Vector pivot{};
                if (!SafeCopy(&parent, &r.bones[k].m_parentIndex, 2)) return WXL_READ_NO_BONES;
                if (!SafeCopy(&pivot, &r.bones[k].m_pivot, sizeof(pivot))) return WXL_READ_NO_BONES;
                if (parents) parents[k] = static_cast<int16_t>(parent);   // 0xFFFF = no parent, as the engine reads it
                if (pivots)
                {
                    pivots[k * 3 + 0] = pivot.x;
                    pivots[k * 3 + 1] = pivot.y;
                    pivots[k * 3 + 2] = pivot.z;
                }
            }
        }
        return WXL_READ_OK;
    }

    int WritePalette(void* instance, const float* palette, uint32_t count)
    {
        if (!palette) return WXL_READ_NO_PALETTE;
        Resolved r;
        const int stage = Resolve(instance, r);
        if (stage) return stage;
        if (count != r.boneCount) return WXL_READ_BONE_COUNT;
        return SafeCopy(const_cast<C44Matrix*>(r.palette), palette, size_t(count) * 64) ? WXL_READ_OK : WXL_READ_NO_PALETTE;
    }
}
