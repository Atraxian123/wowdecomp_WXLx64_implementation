// The core's side of the outline module's engine access (Engine/Source/WXL/Sdk/wxl_api.h): which units to outline,
// which creatures and objects the open quests need, and what the Gx device is doing for the batch being drawn.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Like ModelAccess.cpp and EquipHost.cpp this is a place that knows the object manager, the quest cache and the Gx
// state shadow. The module gets opaque model handles and plain numbers.
#pragma once

#include "Engine/Source/WXL/Sdk/wxl_api.h"

namespace wxl::api::outlinehost
{
    uint32_t Units(WxlOutlineUnit* out, uint32_t max);
    uint32_t QuestTargets(const void** models, uint32_t max);
    uint32_t QuestTurnIns(const void** models, uint32_t max);
    uint32_t TrackedTargets(WxlOutlineTracked* out, uint32_t max, float maxYards);
    int      BatchState(float* alphaRef);
}
