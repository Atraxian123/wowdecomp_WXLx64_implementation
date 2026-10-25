// Guarded reads of the engine's model instances, behind WxlApi::readInstance / readBones / writePalette.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// The only file of the module API that knows what a CM2Model is. A module never sees these types: it gets
// plain numbers, and every read of client memory is done here under SEH, so a stale instance costs one frame
// of one module, not the client.
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"

namespace wxl::api::model
{
    int ReadInstance(void* instance, WxlInstanceView* out);
    int ReadBones(void* instance, uint32_t count, float* palette, int16_t* parents, float* pivots);
    int WritePalette(void* instance, const float* palette, uint32_t count);
}
