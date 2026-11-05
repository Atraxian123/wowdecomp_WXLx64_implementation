// Modern M2 module: what Load.cpp, Skin.cpp and Module.cpp share. Nothing here leaves the module.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The module keeps no per-model state: what used to be wxl::modernm2::State inside CM2Shared now lives in the
// client (Engine/Source/Model2/ModernM2/ModernM2.h) and travels through the structs of wxl_modernm2_api.h.
#pragma once

#include "M2Records.hpp"
#include "wxl_modernm2_api.h"

#include <cstddef>
#include <cstdint>

namespace wxl::m2mod
{
    using namespace wxl::m2rec;

    constexpr uint32_t kVersionMin = WXL_M2_VERSION_MIN;   // inner M2 version window the reader accepts
    constexpr uint32_t kVersionMax = WXL_M2_VERSION_MAX;

    inline bool IsAcceptedVersion(uint32_t version) { return version >= kVersionMin && version <= kVersionMax; }

    constexpr uint32_t kMaxTxid = WXL_M2_MAX_TXID;

    /// The client's vertex shader holds the bone palette from register 31, 3 registers per matrix:
    /// (256 - 31) / 3 = 75 bones per draw.
    constexpr uint32_t kMaxBonesPerDraw = 75;

    // Load.cpp
    int  PrepareLoad(const char* name, const void* file, uint32_t fileSize, const WxlM2Alloc* mem, WxlM2PrepareResult* out);
    void FinishWalk(void* model);
    int  TextureName(uint32_t fileId, char* out, size_t cap);
    int  UnwrapAnim(void* buffer, uint32_t* size);

    // Skin.cpp
    int  FinalizeSkin(const WxlM2Alloc* mem, WxlM2SkinJob* job);
}
