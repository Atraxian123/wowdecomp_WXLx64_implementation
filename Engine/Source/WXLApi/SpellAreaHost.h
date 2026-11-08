// The core's side of the spell area module's engine access (Engine/Source/WXL/Sdk/wxl_api.h): the visible dynamic
// objects and casting units, a spell's effects and radii, the targets of a cast in progress, and the projected
// texture pass the areas are drawn with. See Engine/Source/WXLModules/SpellArea/SpellArea.md.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Like OutlineHost.cpp this is a place that knows the object manager, the spell database and the Gx device. What to
// draw, where it comes from and what it looks like is the module's business (wxl_spellarea.dll).
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"
#include "Engine/Source/WXLApi/SpellAreaRender.h"

#include <cstdint>

namespace wxl::api::spellareahost
{
    uint32_t DynamicObjects(WxlSpellAreaDynObject* out, uint32_t max);
    uint32_t Casters(WxlSpellAreaCaster* out, uint32_t max);
    int      Effects(uint32_t spellId, uint64_t casterGuid, WxlSpellAreaEffects* out);
    int      PendingCast(uint64_t casterGuid, uint32_t spellId, WxlSpellAreaPending* out);
    int      ObjectPose(uint64_t guid, float* pos, float* facing);
    void*    CreateTexture(const uint32_t* argbPixels, uint32_t size, const char* name);
    void     Draw(const WxlSpellAreaDraw* draw);
    void     SetRenderer(WxlSpellAreaRenderFn fn, void* user);
}
