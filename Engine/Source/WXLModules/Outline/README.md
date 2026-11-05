# wxl_outline (module)

Reaction-colored silhouette outline on the mouseover and target units, ported from the standalone
`wxl-unit-outline` extension. Now a module, `WXL\Modules\wxl_outline.dll`, loaded by the client like the others; it
unloads itself when `[Outline] Enabled` and `Quest` are both 0 or absent. Off by default.

```ini
[Outline]
Enabled=1          ; mouseover / target, colored by reaction
Quest=1            ; quest objectives, always on
HostileColor=FF0000
NeutralColor=FFFF00
FriendlyColor=00FF00
QuestColor=00C8FF
QuestThroughWalls=0
HideBehindObjects=0  ; 1 = no outline shows through anything in front
Spread=1.5
Intensity=2.0
```

Hook points it relies on, as module API events: `OnEndScene` and `OnDeviceLost` (both `CGxDeviceD3d` and
`CGxDeviceD3d9Ex`), and `OnM2BatchDraw`, emitted from the device's indexed `Draw()` with the model that
`CM2SceneRender::DrawBatch` names through `WXL/DrawContext.h`. What it needs from the client goes through three calls
of the module API (`outlineUnits`, `outlineQuestTargets`, `outlineBatchState`; the core side is
`Engine/Source/WXLApi/OutlineHost.cpp`) plus `modelInfo` to walk a model's parents; the quest matching rules are in
`Engine/Source/WXLOutline/QuestMatch.h` (engine-free, with a host test: `Tools/WxlModuleTest/run_questmatch_tests.sh`).

More keys:

```ini
Debug=0              ; 1 = also draw the raw silhouette mask in the top-left corner
```

Pipeline, one frame behind: `EndScene` of frame N picks the targets for frame N+1; while frame N+1 draws, every opaque
batch of a picked unit (and of whatever is attached to it) is stamped into a mask render target; `EndScene` of N+1 blurs
the mask, punches the silhouette back out and composites the halo into the frame. Players are outlined only where the
scene does not hide them; NPCs show through objects unless `HideBehindObjects` is on. Quest outlines come from the quest
log and the client's own creature / object caches, refreshed four times a second. `QuestThroughWalls=1` lets quest
outlines show through walls; `HideBehindObjects=1` hides every outline wherever something nearer covers the unit.
