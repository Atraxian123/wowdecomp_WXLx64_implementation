// WXL culling switches: lets WXLExtendedEquipment.ini turn off the ways a WMO can hide other objects.
// Copyright (C) 2026 WarcraftXL (GPLv3, see Event.h)
//
// All keys live in WXLExtendedEquipment.ini (beside the client executable), section [Culling]. Every key
// defaults to 0, which is the stock client's behaviour; nothing changes unless you set one.
//
//     [Culling]
//     ; Master switch. 1 = nothing that belongs to a WMO may hide other objects. Same as setting the
//     ; three keys below to 1.
//     DisableWmoCulling=0
//
//     ; The camera being "inside" a WMO no longer hides the outside world. Stock behaviour: when the
//     ; camera is classified as inside a WMO and its portals show no way out, the whole exterior sort
//     ; table is emptied, which removes every outdoor WMO group, terrain chunk, liquid and M2 object
//     ; (CWorldScene::Render). With this on, the outside is culled like it is when you stand outdoors
//     ; (view frustum and distance only). Interiors still render through their portals as before.
//     DisableViewerWmoCull=0
//
//     ; WMO groups stop adding occluder segments to the horizon buffer (Map.cpp, AddOccluder), so a
//     ; WMO can no longer hide whatever is behind it.
//     DisableWmoOccluders=0
//
//     ; The hardcoded occlusion volumes (CWorldScene::CreateOcclusionVolumes, for example the wall quad
//     ; in Stormwind on map 0) are not built, so nothing is hidden behind them.
//     DisableOcclusionVolumes=0
//
//     ; The horizon buffer (terrain chunks and the occluders above) is not consulted, so nothing is
//     ; hidden behind terrain or occluders. This is what the console command "showCull" does, but from
//     ; start-up. Also covered by the master switch.
//     DisableHorizonBuffer=0
//
//     ; The two remaining tests that can drop a WMO group. They are NOT covered by the master switch,
//     ; because they make the client draw WMO groups that are off screen; they exist to bisect a bug.
//     DisableWmoFrustumCull=0   ; exterior WMO groups are never rejected by the view frustum
//     DisableSceneBoxCull=0     ; WMO groups are never rejected for being outside the scene bounding box
//
//     ; Portal culling. A WMO group flagged indoor is only ever drawn by walking portals: from outside, from
//     ; an exterior group's portals; from inside, from the camera's group. A group with no portal chain to
//     ; one of those (a WMO whose indoor groups lack an outdoor portal) is never drawn, and the M2 doodads
//     ; it places go with it. With this on, every group is culled only by its own bounding box (view
//     ; frustum, occlusion volumes, horizon buffer) and drawn whenever that test passes, portals or not.
//     ; Costs draw calls (indoor groups out of any portal's sight are drawn and hidden by the depth buffer).
//     ; Not part of the master switch.
//     DisablePortalCulling=0
//
//     ; Trace (needs [Log] Enabled=1): record WHY a WMO group is not drawn. The value is a case-insensitive
//     ; part of the WMO file path; "*" matches every WMO (very chatty).
//     TraceWmo=stormwind
//
// The switches are read once, on first use. Edit the ini, restart the client.
//
// Diagnostics: with WXL logging on ([Log] Enabled=1) the category CULL records each switch at start-up,
// a line every time the camera's WMO state changes (see ViewerChanged), and, for TraceWmo, a line every
// time a traced WMO group changes its verdict (see TraceGroup), plus one summary line each time a whole
// traced WMO goes from visible to fully hidden or back. The M2 doodads a WMO places are added only when
// their group passes, so a group's verdict is also theirs.
#pragma once

namespace wxl::culling
{
    /// [Culling] DisableViewerWmoCull (or the master DisableWmoCulling).
    bool KeepExteriorWhenInsideWmo();

    /// [Culling] DisableWmoOccluders (or the master).
    bool DisableWmoOccluders();

    /// [Culling] DisableOcclusionVolumes (or the master).
    bool DisableOcclusionVolumes();

    /// [Culling] DisableHorizonBuffer (or the master).
    bool DisableHorizonBuffer();

    /// [Culling] DisableWmoFrustumCull (not part of the master).
    bool DisableWmoFrustumCull();

    /// [Culling] DisableSceneBoxCull (not part of the master).
    bool DisableSceneBoxCull();

    /// [Culling] DisablePortalCulling (not part of the master): groups are culled by their own bounding box
    /// and never by portal reachability.
    bool DisablePortalCulling();

    // ---- trace: why is a WMO group / M2 object not drawn? ----

    /// How far a WMO group got this frame. Each stage overwrites the previous one for the same group, so
    /// the last verdict of the frame is the reason it is (or is not) drawn. Order of the pipeline:
    /// CMap::PrepareMapObjDefs, then CWorldScene::CullMapObjDefGroups, then
    /// CMapObjDef::CullMapObjDefGroupFromExterior.
    enum class GroupVerdict : int
    {
        NotPrepared = 0,        // the WMO instance is not initialised yet
        NotLoaded,              // the group's file is not loaded yet
        Disabled,               // the WMO instance is flagged disabled (MAPOBJ_FLAG_DISABLED)
        OutsideSceneBox,        // outside the scene bounding box built from the view frustum
        Queued,                 // handed to the exterior sort table
        CulledFrustum,          // exterior pass: outside the view frustum
        CulledVolume,           // exterior pass: inside a hardcoded occlusion volume
        CulledBuffer,           // exterior pass: behind the horizon buffer
        PassedStage1,           // survived the exterior pass
        CulledPortalFrustum,    // stage 2: outside the (portal-narrowed) frustum
        CulledPortalBuffer,     // stage 2: behind the horizon buffer
        RenderedDirect,         // VISIBLE: group flag 0x10000, rendered directly
        RenderedViaPortals,     // VISIBLE: exterior group (flag 0x8), entered through its portals
        NotRenderedFromExterior // survived, but its group flags give the exterior no way to render it
    };

    /// True when [Culling] TraceWmo is set and `wmoPath` matches it. Cheap (false at once when unset);
    /// call it before building anything for TraceGroup.
    bool TraceWmoWanted(const char* wmoPath);

    /// Records this frame's verdict for one WMO group. `key` identifies it (the CMapObjDefGroup pointer);
    /// `groupName` may be null.
    void TraceGroup(const void* key, const char* wmoPath, unsigned groupNum, const char* groupName, GroupVerdict v);

    /// Once per frame, after the culling passes: logs the group verdicts that changed since the last frame,
    /// and the whole-WMO summary lines. Does nothing when no trace is set.
    void TraceFlush(float camX, float camY, float camZ);

    /// What CWorldScene::Render did with the exterior this frame.
    enum class ExteriorAction : int
    {
        Outdoors = 0,      // the camera is not inside a WMO: the exterior is culled normally
        PortalCulled = 1,  // inside a WMO whose portals reach the outside: culled through the portal view
        Cleared = 2,       // inside a WMO with no way out: the exterior sort table was emptied (stock)
        KeptByToggle = 3,  // inside a WMO, but DisableViewerWmoCull kept the exterior
    };

    /// Cheap per-frame test: true only when logging is on and (viewerKey, action) differs from the last
    /// call, i.e. when it is worth building the strings for LogViewer. viewerKey is the viewer's CMapObjDef
    /// pointer (null outdoors); it is only compared, never dereferenced.
    bool ViewerChanged(const void* viewerKey, ExteriorAction action);

    /// Writes the CULL line for a change reported by ViewerChanged. `wmo` and `groups` are free text
    /// (the WMO file name; the group numbers, names and flags the camera is in); either may be null.
    void LogViewer(ExteriorAction action, const char* wmo, const char* groups, float x, float y, float z);
}
