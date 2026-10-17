// Typed access to the engine objects the equip extension drives.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The injected build reached these objects through raw 32-bit offsets (cmo+0x38, inst+0x2C, hdr+0x30 ...)
// and called engine functions by address. In this tree every one of them is a real member or function,
// and the offsets are wrong on x64 anyway (pointers are 8 bytes), so this header is the single place
// that knows how the extension's "cmo / sceneNode / renderCtx / model" vocabulary maps onto the decomp:
//
//   cmo        CCharacterComponent*   (the item-slot event's charModelObj)
//   sceneNode  CM2Model*              cmo->m_data.m_model, the character's own model instance
//   owner28    CM2Scene*              sceneNode->m_scene, which owns the model cache
//   renderCtx  CM2Model*              one attached model instance
//   model      CM2Shared*             the loaded .m2 behind an instance (renderCtx->m_shared)
//
// Everything takes and returns void* so the extension's logic reads as it did before the port.
#pragma once

#include "Engine/Source/Base/Status.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Common/ComponentCore/CharacterComponent.h"
#include "WoW/Source/Glue/ComponentData.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wxl::equip::eng
{
    /// One bone matrix in the palette (CM2Model::m_boneMatrices is a C44Matrix array).
    constexpr size_t kBonePaletteStride = sizeof(C44Matrix);
    static_assert(sizeof(C44Matrix) == 0x40, "bone palette stride is 64 bytes");

    // --- CCharacterComponent ("cmo") ---

    inline CM2Model* CmoModel(void* cmo)
    {
        return cmo ? static_cast<CCharacterComponent*>(cmo)->m_data.m_model : nullptr;
    }
    inline uint32_t CmoRace(void* cmo)
    {
        return cmo ? static_cast<CCharacterComponent*>(cmo)->m_data.m_preferences.m_raceID : 0;
    }
    inline uint32_t CmoGender(void* cmo)
    {
        return cmo ? static_cast<CCharacterComponent*>(cmo)->m_data.m_preferences.m_sexID : 0;
    }

    // --- CM2Model ("sceneNode" / "renderCtx") ---

    /// The scene that owns the model cache; what GetRenderCtx must be called on.
    inline void* ModelScene(void* model)
    {
        return model ? static_cast<CM2Model*>(model)->m_scene : nullptr;
    }
    /// Init flags: bit 0 = animation data ready, bit 18 (0x40000) = attached to root with force.
    inline uint32_t ModelFlags(void* model)
    {
        return model ? static_cast<CM2Model*>(model)->m_fFlags : 0;
    }
    /// The instance's bone matrices, or null before its animation data exists.
    inline uint8_t* ModelBonePalette(void* model)
    {
        return model ? reinterpret_cast<uint8_t*>(static_cast<CM2Model*>(model)->m_boneMatrices) : nullptr;
    }
    /// The loaded .m2 behind an instance. The model destructor nulls it, which is the extension's
    /// "this render context is dead" test.
    inline void* ModelShared(void* model)
    {
        return model ? static_cast<CM2Model*>(model)->m_shared : nullptr;
    }
    /// The parsed .m2 header of an instance's model, or null when not loaded yet.
    inline M2Data* ModelHeader(void* model)
    {
        CM2Shared* shared = model ? static_cast<CM2Model*>(model)->m_shared : nullptr;
        return shared ? shared->m_data : nullptr;
    }

    // --- CM2Shared ("model") ---

    /// Model path as the cache keyed it. Always a valid, NUL-terminated buffer.
    inline const char* SharedPath(void* shared)
    {
        return shared ? static_cast<CM2Shared*>(shared)->m_fileName : "";
    }
    /// The skin profile, or null before it is attached. Index and section arrays are relative
    /// (M2Array), so they are x64-safe as stored.
    inline M2SkinProfile* SharedSkin(void* shared)
    {
        return shared ? static_cast<CM2Shared*>(shared)->m_skinData : nullptr;
    }

    // --- Engine calls (formerly gm2:: calls by address) ---

    /// Gets or creates a model instance by path in a scene (0x0081F8F0, CM2Scene::CreateModel).
    inline void* GetRenderCtx(void* scene, const char* path)
    {
        return scene ? static_cast<CM2Scene*>(scene)->CreateModel(path, 0) : nullptr;
    }
    /// Attaches child to parent at an attachment id (0x00831630, CM2Model::AttachToParent).
    /// force bypasses the "attachment point missing -> silently do nothing" exit.
    inline void AttachToScene(void* child, void* parent, uint32_t attachId, bool force)
    {
        static_cast<CM2Model*>(child)->AttachToParent(static_cast<CM2Model*>(parent), attachId, nullptr, force ? 1 : 0);
    }
    /// Detaches every child of parent attached at attachId (0x00827560, DetachAllChildrenById).
    inline void DetachSlot(void* parent, uint32_t attachId)
    {
        static_cast<CM2Model*>(parent)->DetachAllChildrenById(attachId);
    }
    /// Drops one reference to an instance (0x00824ED0, CM2Model::Release).
    inline void ReleaseRenderCtx(void* model)
    {
        static_cast<CM2Model*>(model)->Release();
    }
    /// Binds a texture to replaceable texture type 2 (0x00825260, CM2Model::ReplaceTexture).
    inline void BindTexSlot(void* model, void* texture)
    {
        static_cast<CM2Model*>(model)->ReplaceTexture(2, static_cast<CTexture*>(texture));
    }
    /// Loads a texture by path (0x004B9760, TextureCreate) with all-zero texture flags, as before.
    inline void* LoadResource(const char* path)
    {
        CGxTexFlags flags;
        std::memset(&flags, 0, sizeof(flags));
        CStatus status;
        return TextureCreate(path, flags, &status, 0);
    }
}
