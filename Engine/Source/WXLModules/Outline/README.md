# wxl_outline (module)

Reaction-colored silhouette outline on the mouseover and target units, ported from the standalone
`wxl-unit-outline` extension. Now a module, `WXL\Modules\wxl_outline.dll`, loaded by the client like the others; it
unloads itself when `[Outline] Enabled`, `Quest`, `QuestTurnIn` and `Tracked` are all 0 or absent. Off by default.

```ini
[Outline]
Enabled=1          ; mouseover / target, colored by reaction
Quest=1            ; quest objectives, always on
HostileColor=FF0000
NeutralColor=FFFF00
FriendlyColor=00FF00
QuestColor=00C8FF
QuestTurnIn=1      ; also outline the NPCs / objects where a completed quest is handed in
QuestTurnInColor=FFD200
Tracked=1          ; also outline what the minimap tracks (nodes, tracked creatures, tracking-menu NPCs)
TrackedRange=80    ; yards from you, 0 = every visible one
QuestThroughWalls=0
HideBehindObjects=0  ; 1 = no outline shows through anything in front
TransparentUnits=0   ; 1 = also outline see-through models (ghosts, spirits); they are skipped by default
TransparentAlphaRef=0.05  ; texture alpha below this is left out of a see-through model's outline, 0..1
Spread=1.5
Intensity=2.0
```

Hook points it relies on, as module API events: `OnEndScene` and `OnDeviceLost` (both `CGxDeviceD3d` and
`CGxDeviceD3d9Ex`), and `OnM2BatchDraw`, emitted from the device's indexed `Draw()` with the model that
`CM2SceneRender::DrawBatch` names through `WXL/DrawContext.h`. What it needs from the client goes through three calls
of the module API (`outlineUnits`, `outlineQuestTargets`, `outlineBatchState`; the core side is
`Engine/Source/WXLApi/OutlineHost.cpp`) plus `modelInfo` to walk a model's parents; the quest matching rules are in
`Engine/Source/WXLOutline/QuestMatch.h` (engine-free, with a host test: `Tools/WxlModuleTest/run_questmatch_tests.sh`).

Quest turn-ins: with `QuestTurnIn=1` every visible NPC or object that is waiting for you to hand in a completed quest
gets an outline in `QuestTurnInColor` (gold by default, like the "?" over its head). It does not need `Quest=1`. The
list is the server's own: the client keeps the quest giver status the server sends for your quests on each NPC (the
yellow "?" of a completed quest, the same for a quest that no longer gives experience, and the blue "?" of a
repeatable one), and the outline follows it, so it also covers a quest that is handed in to somebody other than its
giver. Dead units and players are skipped. A unit that is a mouseover / target keeps the reaction color, and one that
is also a quest objective keeps `QuestColor`. `QuestThroughWalls` and `HideBehindObjects` apply to it as well.

Tracked things: with `Tracked=1` the outline marks what your minimap is tracking, by the client's own rules for a
minimap blip, so it follows whatever you switch on. Three kinds, each with its own switch and color:

| kind | what the client tracks | switch | color |
|---|---|---|---|
| objects | resource nodes of the tracking spells (Find Minerals, Find Herbs, Find Treasure, ...: the node's lock type against your resource tracking), and the mailboxes of the tracking menu | `TrackedObjects=1` | `TrackedObjectColor=FFA040` |
| creatures | the creature types of the tracking spells (Track Beasts, Humanoids, Undead, ...), units the server flags as tracked, and hidden units while you detect them | `TrackedCreatures=1` | `TrackedCreatureColor=FF40FF` |
| NPCs | the NPCs of the tracking menu: repair, vendors, trainers, innkeeper, flight master, stable master, battle master, auctioneer, banker | `TrackedNpcs=1` | `TrackedNpcColor=FFFFFF` |

Only visible units that are alive (or can be used dead), not your own pet and not players, are outlined. They come after
every other outline: a unit that is the mouseover / target, a quest objective or a quest turn-in keeps that color, and
the tracked ones are the first dropped if the list is full. `TrackedRange` (default 80 yards, 0 = no limit) keeps a
far node from being outlined across the map; `TrackedThroughWalls` (default: the value of `QuestThroughWalls`) lets
them show through walls, and `HideBehindObjects` applies as for the others. The trivial-quests entry of the tracking
menu changes quest icons only, so it has nothing to outline.

Hiding behind walls (`QuestThroughWalls=0`, `TrackedThroughWalls=0`, `HideBehindObjects=1`) tests the outline against the
scene's depth buffer. The silhouette is the unit's own triangles drawn again right after the engine drew them, so on its
own pixels the depth only just matches; the module therefore follows the engine's depth convention, accepts equal depth,
pulls the redraw a hair toward the camera and ignores the engine's stencil test for that draw. `DepthTolerance=1` scales
that hair: raise it (2, 4, ...) if outlines are still missing with the walls switched on although nothing is in the
way, lower it (0.25, 0) if outlines show through thin things that are right in front of the unit. The first depth-tested
draw writes one `depth test on: ...` line to WXL.log with the depth buffer and the engine's depth function.

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
