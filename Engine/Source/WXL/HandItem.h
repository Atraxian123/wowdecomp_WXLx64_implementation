// Hook point for held-weapon model resolution (CCharacterComponent::AddHandItem).
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::hooks
{
    /**
     * @brief Callback type: may replace the model file name the client loads for a held weapon.
     * @param displayId  the ItemDisplayInfo id of the weapon.
     * @param invSlot    the WoW inventory slot being filled (15 = main hand, 16 = off hand, 17 = ranged).
     * @param out        receives the replacement file name (bare name, no directory: the client prepends
     *                   Item\ObjectComponents\Weapon\ or ...\Shield\ itself).
     * @param outSize    capacity of out.
     * @return true when out holds a replacement the client must load instead of the record's own Model1.
     */
    using HandItemModelResolver = bool (*)(uint32_t displayId, uint32_t invSlot, char* out, size_t outSize);

    /// Registers the resolver. One resolver only; a later call replaces the earlier one.
    void SetHandItemModelResolver(HandItemModelResolver fn);

    /// Called by AddHandItem before it builds the model path. false when no resolver is set or it declines.
    bool ResolveHandItemModel(uint32_t displayId, uint32_t invSlot, char* out, size_t outSize);
}
