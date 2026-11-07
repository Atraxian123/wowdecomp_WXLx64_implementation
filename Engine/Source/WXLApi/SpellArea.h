// The spell area indicator's engine side. See Engine/Source/WXL/SpellArea.md.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// Draws the ground area of spells being cast, channeled or lying on the ground (a dynamic object) as colored,
// terrain-hugging circles, cones and lines, with the client's own projected texture pass (the one that draws the
// spell targeting circle under the cursor). Off unless [SpellArea] Enabled=1 in WXLExtendedEquipment.ini.
#pragma once

#include <cstdint>

namespace wxl::spellarea
{
    /// Called by the world render beside the client's own spell cursor shadow, once after the opaque pass with
    /// (0x200122, 5) and once after the transparent ones with (0x20000, 4): the ProjectTex triangle flags and
    /// render flags of those two calls (the second leaves the doodads out so nothing is drawn twice).
    /// Does nothing while the indicator is off.
    void Render(uint32_t triFlags, uint32_t projectFlags);
}
