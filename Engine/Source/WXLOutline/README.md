# WXLOutline

Reaction-colored silhouette outline on the mouseover and target units, ported from the standalone
`wxl-unit-outline` extension into the client. Off by default; see `Outline.h` for the ini keys.

```ini
[Outline]
Enabled=1          ; mouseover / target, colored by reaction
Quest=1            ; quest objectives, always on
QuestColor=00C8FF
QuestThroughWalls=0
HideBehindObjects=0  ; 1 = no outline shows through anything in front
Spread=1.5
Intensity=2.0
```

Engine hook points it relies on: `OnEndScene` and `OnDeviceLost` (both `CGxDeviceD3d` and `CGxDeviceD3d9Ex`),
and `OnM2BatchDraw`, emitted from the device's indexed `Draw()` with the model that
`CM2SceneRender::DrawBatch` names through `WXL/DrawContext.h`.
