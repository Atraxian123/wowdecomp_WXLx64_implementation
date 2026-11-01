// WXL unit outline: reaction-colored silhouette outline on the mouseover and target units.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Engine/Source/WXL/Event.h)
//
// OPT-IN. Off unless WXLExtendedEquipment.ini (beside the client executable) says:
//
//     [Outline]
//     ; 1 = outline the mouseover and target units, colored by reaction (red hostile, yellow neutral, green friendly).
//     Enabled=1
//     ; 1 = outline, all the time, what your open quests still need: creatures to kill, objects to use, and the
//     ;     creatures and objects that can hold a quest item you are still missing. Works with or without Enabled.
//     Quest=0
//     ; Outline colors, RRGGBB hex. An absent or malformed key keeps the default shown.
//     ; The mouseover / target outline, by the unit's attitude toward you:
//     HostileColor=FF0000
//     NeutralColor=FFFF00
//     FriendlyColor=00FF00
//     ; The quest outline:
//     QuestColor=00C8FF
//     ; 1 = quest outlines show through walls. 0 = the scene hides them where it hides the thing.
//     QuestThroughWalls=0
//     ; 1 = every outline (mouseover, target, quest) is hidden wherever something nearer covers the unit or object,
//     ;     the player model and walls included. Overrides QuestThroughWalls. 0 = NPC outlines show through.
//     HideBehindObjects=0
//     ; Blur tap spacing in frame pixels: the width of the halo. 1.5 is the shipped look.
//     Spread=1.5
//     ; Halo opacity multiplier.
//     Intensity=2.0
//     ; 1 = also draw the raw silhouette mask in the top-left corner (what was stamped, before blurring).
//     Debug=0
//
// Mouseover and target: players are outlined only where the scene does not hide them; NPCs show through objects
// unless HideBehindObjects is on. The halo is a blur, so a few pixels of it can still spill onto whatever is in front.
// Quest: what the quest log and the client's own creature / object caches say is still needed, refreshed four times a
// second; a creature or object the client has not cached yet is picked up once it is. Read once, at startup.
//
// Pipeline, one frame behind: EndScene of frame N picks the targets for frame N+1; while frame N+1 draws, every
// opaque batch of a picked unit (and of whatever is attached to it) is stamped into a mask render target;
// EndScene of N+1 blurs the mask, punches the silhouette back out and composites the halo into the frame.
#pragma once

namespace wxl::outline
{
    /// Reads the ini and, when [Outline] Enabled=1 or Quest=1, brings the outline up. Call once at startup after wxl::InitModules().
    void Init();
}
