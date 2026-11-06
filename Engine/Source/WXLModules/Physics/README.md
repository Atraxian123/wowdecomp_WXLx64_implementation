> **Physics is a WXL module now**: `wxl_physics.dll`, built from this folder, loaded from `WXL\Modules\` by the
> 64-bit client. It sees the client only through `Engine/Source/WXL/Sdk/wxl_api.h` (see the README there).
> Tests: `Tools/PhysicsGuardTest` (the engine and the guard) and `Tools/WxlModuleTest` (the module through the C ABI).

# Third-party: Kirazul's port of retail's physics engine

`physics_domino.h` and `physics_domino.cpp` are copied unchanged from
[Kirazul/wow-retail-backport](https://github.com/Kirazul/wow-retail-backport)
(`scripts/ref/`, commit d086009), with the author's permission. They are Kirazul's
port of the retail client's runtime physics engine ("Domino"), read from the
retail client; see the files' own headers and the kit's `references/rules.md`
section 6. They are not covered by this repository's GPL licence statement:
the copyright position of the original engine is Blizzard's, and the port is
Kirazul's work.

Equip Plus only adds the glue that feeds the client's bone matrices in and
writes the simulated bones back (`../PhysicsLive.cpp`).

---

# In-tree integration (this repository)

`physics_domino.{h,cpp}` above are unchanged. Everything else in this folder is the glue written
for the decompilation:

| File | What it is |
| --- | --- |
| `Phy2Library.{hpp,cpp}` | Engine-free: finds, reads, parses and caches one model's `.phy2`. Unit-testable. |
| `PhysicsLive.{hpp,cpp}` | The hook: reads the bone palette through real `CM2Model` / `CM2Scene` / `CM2Shared` members, steps the engine, writes the simulated bones back. |

## One `.phy2` per model, beside the model

Physics is **not** one table for the whole game. The `.phy2` for a model sits next to it, same folder and name:

```
item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.m2
item\objectcomponents\weapon\staff_2h_raidmidnight_d_02.phy2
```

Put it wherever the client can open a file by that name: inside a patch MPQ, or loose under `Data\` (the
reader tries archives first, then archives plus loose files, like the equip extension's native reader).
A model with no `.phy2` is left alone, and the answer is remembered, so it costs one lookup per model.

**Virtual paths.** When the client is handed a virtual path (`VirtualPath.hpp`), the `.phy2` of the
*original* `.m2` is used:

| Path the client reports | `.phy2` looked for |
| --- | --- |
| a normal model `a\b\c.m2` | `a\b\c.phy2` |
| collection key `a\b\c_wxl_..._cmo<hex>.m2` | `a\b\c.phy2` |
| global key `a\b\c[_oh]_<displayId>.m2` | `a\b\c.phy2`, and `a\b\c_oh.phy2` is tried first |

The global-key rule only applies when `VPathIsVirtualKey()` confirms the path was minted by VirtualPath.
A real model whose name merely ends in `_<digits>` (retail has many: `staff_x_01`, `staff_x_02` are different
models) never borrows a sibling's physics.

## Making the files

`Tools/m2_extract_phys.py` (retail `.m2` -> `.phys` -> `.phy2`, window or command line):

```
python Tools/m2_extract_phys.py Dagger.m2 --phy2        -> Dagger.phys + Dagger.phy2
python Tools/m2_extract_phys.py Dagger.phys             -> Dagger.phy2
```

## Switching it on

`WXLExtendedEquipment.ini`, beside the game executable:

```ini
[Physics]
Enabled=1
```

Log category `PHYS` (`WarcraftXL` log): `MATCH '<model>' <- <file>.phy2` when a model gets physics,
`no .phy2 for item model` for item models without one, `names bone N but the model has M` when a `.phy2`
does not fit the model the client loaded (for example after a converter merged bones).

## How it hooks in

`Equip::Init()` calls `wxl::equip::physics::Init()` **after** the equip extension's `Create()`. It subscribes to
`OnBuildBonePalette` (end of `CM2Model::AnimateMT`, original `0x0082F0F0`). Subscribers run in subscription
order, so physics sees the palette after the equip remap and offhand mirror. The event can fire on animate
threads; the file lookup runs outside the global lock so a disk read never stalls other threads.

---

# Stuck items (jump, stairs): PhysicsGuard

`PhysicsGuard.{hpp,cpp}` is engine-free (no client types). `Tools/PhysicsGuardTest/physics_guard_test.cpp` drives
the real engine with a character that walks, jumps, climbs stairs and teleports (build line at the top of the file).

**Sleeping is not the cause.** `physics_domino.cpp` never puts an item world to sleep: every body is made with
`allowSleep = false`, so the sleep rule can never fire and `islandAwake` is only ever set to true. There is no
sleeping position to wake.

**What does get an item stuck (reproduced offline).** A model with *several* kinematic anchors and no root body
(`BDY4` group `0x4000`) is carried through its first anchor only. A second anchor teleports with its bone, but the
dynamic bodies hanging from it are not carried (`if (!head && S.root < 0) continue;` in `Instance::Step`), so the
joints must drag them. During a jump or a flight of stairs the bone moves faster than the joint solver follows, and
that chain trails 1 yd or more behind (tip 1.28 yd off for a 0.75 yd chain on a jump, 1.45 yd at 20 fps).
`CarryAnchors=1` gives every such anchor the head group (`0x8000`): same scenarios, 0.15 to 0.24 yd.

**The safety net.** Each frame the simulated bones are compared with the animated ones. A bone that stays farther
than `UnstickReach` (default 300%) of its distance to the nearest anchor (plus 0.15) for 0.25 s of simulated time
is stuck or flung; a placement that moves more than `TeleportYards` (default 6) in one frame is a teleport. Either
way the palette is left as the engine built it, the instance is dropped and the next frame re-seeds it from the
animated pose. Four re-seeds in 5 s switch that instance's physics off for 10 s, so it can never loop.

```ini
[Physics]
Enabled=1
CarryAnchors=1
Unstick=1
UnstickReach=300
TeleportYards=6
```

Log lines (category `PHYS`, written only when `[Log] Enabled=1` is set in `WXLExtendedEquipment.ini`): `has several anchors and no root body: N anchor(s) now carry their followers`,
`stuck '<model>': ... re-seeding (bone N at X% of its limit)`, `teleport '<model>': ...`, and
`re-seeded 4 times ...: physics off for it for 10 s`. `GetStats()` counts `unstuck`, `teleports`, `disabled`
and `modelsCarried`. If the unstick line never appears and an item still looks stuck, the cause is not one of
these, and the model named in the log is the one to look at.

---

# Tuning the physics from the ini (global multipliers)

`WXLExtendedEquipment.ini`, section `[Physics]`. Every key is a multiplier on what the `.phy2` says, applied to every
model at once, so the movement can be tuned without editing the files. `1.0` = as the file has it; a key that is
missing or blank is `1.0`. Decimals can be written `1.25` or `1,25`. A value outside a key's range is clamped to it
and a value that is not a number is ignored; both are logged (category `PHYS`).

```ini
[Physics]
Enabled=1

GravityScale=1.0          ; pull down. 0 = the parts float where the model puts them. Negative (to -100): they rise
LinearDampingScale=1.0    ; drag on movement. Up: settles sooner, gooier. Down: swings longer
AngularDampingScale=1.0   ; drag on turning/twisting
InertiaScale=1.0          ; resistance to being rotated (0.01 .. 100). Up: lazier turns
MassScale=1.0             ; weight of the parts (shape density, 0.01 .. 100)
FrictionScale=1.0         ; grip against the collider (result 0 .. 10)
RestitutionScale=1.0      ; bounce (result 0 .. 1)
SwingLimitScale=1.0       ; shoulder-joint cone (result 10 .. 170 degrees)
TwistLimitScale=1.0       ; shoulder twist and revolute range, scaled about their centre; 0 = a tight ~0.5 degree limit
JointStiffnessScale=1.0   ; joint spring frequency (only where the file has a motor torque)
JointDampingScale=1.0     ; joint spring damping (same)
MotorTorqueScale=1.0      ; strongest motor push (same)
FollowScale=1.0           ; how much of a fast move the anchors take at once (carry, result 0 .. 1)

WatchIni=0                ; 1 = re-read these keys whenever the ini is saved, no restart (parts snap back for a moment)
```

Ranges are 0 .. 100 unless noted (`GravityScale`: -100 .. 100). What each key does to what is in the file:

| Key | Changes | On |
| --- | --- | --- |
| GravityScale, LinearDampingScale, AngularDampingScale, InertiaScale | body gravity, linear/angular damping, inertia | dynamic bodies |
| MassScale | shape density | shapes of dynamic bodies |
| FrictionScale, RestitutionScale | shape friction, restitution | every shape |
| SwingLimitScale | shoulder cone | shoulder joints |
| TwistLimitScale | twist range / revolute range, about its centre (a limit never scales to "none") | shoulder, revolute joints |
| JointStiffnessScale, JointDampingScale, MotorTorqueScale | spring Hz, spring damping, motor torque | shoulder, revolute, weld joints |
| FollowScale | carry | kinematic bodies |

Notes:

* **The three joint-spring keys scale numbers the file already has.** A joint whose file value is a motor torque of
  0 (most of them, including the staff `staff_2h_raidmidnight_d_02`) has no motor, and `0 x anything = 0`, so
  these keys change nothing for it. Use them on models whose `.phy2` has motor values.
* **Gravity changes how fast and how far the parts fall; where they come to rest is also set by the joint limits
  and the collider.** Damping is the most direct way to make movement looser or tighter.
* **The limits are soft under load**, in the engine as in the client: a hard shake can push a joint past its cone
  for a moment (the unscaled model does too). Resting poses obey the scaled limit.
* **Extremes are safe.** Every key from 0 to 100, alone or all at once, was run through a shake and 12 s of rest on the
  real staff with no non-finite or exploded state. Very low or very high values still look odd, naturally.
* The multipliers are applied once per model when it is first used, after the guard's own preparation
  (`PhysicsGuard::PrepareModel`). `WatchIni=1` or `PhysicsLive::ReloadFiles()` applies them again.
* **A leading `-` is now read as a sign** in every key (it used to be refused as "not a number"): a negative value is
  clamped to the key's lower limit and logged, which for everything but `GravityScale` is 0 (or the key's minimum).
* **Gravity can be negative.** `GravityScale=-1` turns it around: loose parts rise and stand above their anchor
  instead of hanging. A part that rests exactly in line with its anchor needs a nudge to tip over (a walking
  character gives it one). Test with a negative value before relying on it: the stuck-item guard may re-seed an
  inverted weapon that fights its joint limits, and then it falls back to its animated pose for a while.
* Log lines: `physics: multipliers: GravityScale=0.5 ...` at start-up and on every re-read; `physics: ini: <key> ...`
  for a bad value. `Stats::modelsTuned` counts the models a multiplier changed.

## Underwater multipliers

A second section, `[Physics.Underwater]`, holds **the same multipliers** (every key above except `WatchIni`). They are
applied **on top of** the effective values of a model (the `[Physics]` ones, or the `PhysicsOverride.csv` row of its
display where it has one), and **only while the model belongs to a swimming unit**: a creature or player, an item in
its hand or on its back, a rider on a mount. Out of the water nothing changes.

```ini
[Physics.Underwater]
; Enabled=0 switches the section off without deleting it (default 1). Comments go on their own line.
Enabled=1
; the effective multiplier is (global or CSV value) x (this one): 1.0 x -0.2 = -0.2, so the parts drift up
GravityScale=-0.2
LinearDampingScale=6
AngularDampingScale=6
; seconds the change takes after the unit enters or leaves the water (0 = at once, default 0.4, max 10)
BlendSeconds=0.4
```

* **"Swimming" is the client's own swim state**, the one that picks the swim animations (`CGUnit_C::UpdateSwimmingStatus`,
  the swim bit that is set as soon as the unit is deep enough). It is the same for the local player and for every
  other unit, and it is not a world query: the unit reports it, and the module reads that.
* **An item counts as swimming when the unit it hangs on does** (the walk goes up the attach chain: weapon -> character
  -> mount), so a cloak and a staff change together with their owner.
* **Smooth, with no jump.** The multipliers move linearly from 1 to the underwater value over `BlendSeconds`, in 8
  steps. Each step is a tuned copy of the model for that display, built the first time it is needed and kept; a running
  instance is switched to it with `Instance::Retune`, which keeps its pose, velocities and joint state (only the
  constants change: gravity, damping, inertia, density and so on). The blend is linear in the multiplier, so a negative
  gravity is crossed without a jump. A unit that appears already in the water starts with the underwater values.
* **The underwater values multiply a CSV row's values too**, they do not replace them: a display with `GravityScale=-1`
  in the CSV and `-0.5` here gets `0.5` underwater.
* **Ranges are the same as above** (`GravityScale` -100 .. 100, the rest as listed), and the product is clamped to the
  range too. A key that is missing is 1. If every key is missing (or `Enabled=0`), nothing is done underwater.
* **Gravity is a multiplier on each body's own file value**, so a body whose `.phy2` gravity is 0 stays at 0 whatever
  the scale: a negative `GravityScale` lifts only the bodies that have gravity.
* **Needs the core's `instanceSwimming` call** (`WxlApi`, checked at start-up). With an older core the section is
  read and logged but not applied, and the log says so.
* Log lines: `physics: underwater multipliers (on top of the above): ...` at start-up and on every re-read, and
  `physics: '<model>' enters / leaves the water (blend step n of 8)` for the first 40 transitions.
* `WatchIni=1` re-reads this section too.

# Different physics for single items: PhysicsOverride.csv

The multipliers above apply to every model. `PhysicsOverride.csv` sets some of them again for **one item display**
(`ItemDisplayInfo` id), so two displays that share a model file can move differently: one with dangling bits as
the `.phy2` has them, the other with gravity turned around. It is optional: without the file nothing changes.

```csv
# Kind,DisplayId, then any of the [Physics] multiplier names (any order, any subset)
Kind,DisplayId,GravityScale,SwingLimitScale,MassScale,Note
Item,12345,-1,,,upside-down variant of the staff
Item,12346,,1.5,0.5,floppier and lighter
Creature,6789,0.5,,,a creature (or a mount) with half gravity
```

* **Where:** `PhysicsOverride.csv` beside the client, or `DBFilesClient\PhysicsOverride.csv` beside the client; then,
  through the client's reader, `DBFilesClient\PhysicsOverride.csv` or `PhysicsOverride.csv` in `Data\` or an
  archive (so a patch can ship one). The first one found is used.
* **Header:** the first line that is not empty and not a comment. It needs `Kind` and `DisplayId`; the rest are
  multiplier names exactly as in the ini (`GravityScale`, `SwingLimitScale`, ...). Names are not case sensitive.
  A column named `Note`, `Notes`, `Comment`, `Comments`, `Name` or `Description` is ignored silently; another
  unknown column is ignored with one log line.
* **Kind:** `Item` or `Creature`. Item rows apply to held items: main hand, off hand, ranged, shield. Creature rows
  apply to the model of a unit (a creature, a pet, a shapeshifted player) and to the model of a mount, and need the
  creature's model to have a `.phy2` like any other model. An item id and a creature id never meet, even if the
  numbers are equal.
* **Values:** an empty cell keeps the global value. A filled cell **replaces** the global value for that display
  (`GravityScale` `-1` is -1, not -1 times the ini value). The ranges of the ini keys apply (a value outside is
  clamped and logged); `GravityScale` may be negative.
* **Separators:** a comma. If the header line has semicolons and no commas, the file uses `;` (a spreadsheet in an
  Italian locale writes that) and a decimal comma (`1,25`) works too. A cell can be put in double quotes.
  Lines starting with `#` or `//` are comments. A UTF-8 byte order mark at the start of the file (a spreadsheet adds one) is ignored.
* **Mistakes** skip or trim that row and are logged (category `PHYS`, with the line number); the rest still works.
  A display listed twice: the later row wins.
* **Reload:** `WatchIni=1` also re-reads this file when it is saved (checked once a second, parts snap back for a
  moment); `PhysicsLive::ReloadFiles()` does too.
* **Log lines:** `physics: <file>: N item row(s), M creature row(s)` when the file is read;
  `physics: item display 12345 on '<model>' uses its PhysicsOverride.csv row: GravityScale=-1` (or `creature display`)
  the first time a row changes a model. `Stats::overrideRows` and `Stats::modelsOverridden` count them.

## How the module knows a display

* **A held weapon the equip extension attached.** `OnHandItemAttached` says which display went into which slot;
  `model` in that event is the wearer, so the weapon's own instance is found as the wearer's child at the attach
  link whose file stem is the display's model (`findAttachedChild`). This is what makes two displays that share
  one model file and carry **no** texture or geoset change tell apart. It is checked on every use: the instance
  must still be that wearer's child, so a recycled instance address cannot inherit a stale display.
* **A creature's or a mount's model.** `OnCreatureModelBuilt`, fired by `CGUnit_C::UpdateDisplayInfo` (the unit's
  own model) and `CGUnit_C::CreateUnitMount` (its mount's), names the model instance directly together with the
  `CreatureDisplayInfo` id it was built for, so two displays that share one model file tell apart here too. A
  creature binding is trusted while the instance still shows the file it was built from; a model that took over
  the address of a freed one has another file and is ignored. Binding costs nothing per spawn: the module only
  keeps a small map, looked through for dead entries each time it has doubled. A unit that changes display
  (shapeshift, `.morph`) gets a new model and a new event, so the new display's row applies at once.
* **A baked variant.** When a variant is baked (a texture or geoset change), its model path is
  `<stem>_<displayId>.m2`, which the core decodes (`vpathDecodeGlobalVirtualKey`). Item and creature variants have
  the same shape, so the folder decides: `item\` is an item display, `creature\` a creature display, anything
  else has none. A binding from an event wins over a decoded path.
* Not covered yet: items that are not held (helm, shoulders, cape...) unless they happen to be baked variants;
  corpses (`CGCorpse_C` builds its own creature model); and units whose model is a character model (a humanoid with
  `CreatureDisplayInfoExtra`): its `.phy2` lookup and bone mirroring follow the character path.
