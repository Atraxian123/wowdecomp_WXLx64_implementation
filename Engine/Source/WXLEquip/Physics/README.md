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

GravityScale=1.0          ; pull down. 0 = the parts float where the model puts them
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

Ranges are 0 .. 100 unless noted. What each key does to what is in the file:

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
* Log lines: `physics: multipliers: GravityScale=0.5 ...` at start-up and on every re-read; `physics: ini: <key> ...`
  for a bad value. `Stats::modelsTuned` counts the models a multiplier changed.
