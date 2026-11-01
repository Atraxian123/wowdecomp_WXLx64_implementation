// WXL unit outline: reaction-colored silhouette outline on the mouseover and target units.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// OPT-IN. Off unless WXLExtendedEquipment.ini (beside the client executable) says:
//
//     [Outline]
//     Enabled=1
//     ; Blur tap spacing in frame pixels: the width of the halo. 1.5 is the shipped look.
//     Spread=1.5
//     ; Halo opacity multiplier.
//     Intensity=2.0
//     ; 1 = also draw the raw silhouette mask in the top-left corner (what was stamped, before blurring).
//     Debug=0
//
// Red for hostile, yellow for neutral, green for friendly (the unit's own attitude toward the player). Players are
// outlined only where the scene does not hide them; NPCs show through walls. Read once, at startup.
//
// Pipeline, one frame behind: EndScene of frame N picks the targets for frame N+1; while frame N+1 draws, every
// opaque batch of a picked unit (and of whatever is attached to it) is stamped into a mask render target;
// EndScene of N+1 blurs the mask, punches the silhouette back out and composites the halo into the frame.
#pragma once

namespace wxl::outline
{
    /// Reads the ini and, when [Outline] Enabled=1, brings the outline up. Call once at startup after wxl::InitModules().
    void Init();
}
