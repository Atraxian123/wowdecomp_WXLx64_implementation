# Spell area indicator

Draws the ground area of spells as colored, terrain-hugging shapes: while a spell is **being cast** (the precast),
while it is **channeled**, and for as long as it **lies on the ground** (Blizzard, Flamestrike, Consecration, Death and
Decay, Rain of Fire ...). Circles, cones and lines. Off by default. It is a module, `wxl_spellarea.dll`, loaded from
`WXL\Modules` like the equip, creature and outline modules (a build of the client builds it).

It uses the client's own projected-texture pass, the one that paints the targeting circle under the cursor, so the
shapes follow terrain, WMOs and doodads and show over water. The client's circle while you aim is untouched.

## Turning it on

```ini
[SpellArea]
Enabled=1
```

The ini is looked at once a second: save it and the indicator switches on or off, and picks up the other keys, without
restarting. The module stays loaded while it is off (a frame then costs one compare), which is what makes that possible.
To not load it at all, delete `wxl_spellarea.dll` or list it in `[Modules] Disable=wxl_spellarea.dll`.

## Keys (all optional)

```ini
[SpellArea]
Enabled=1
Own=1                   ; your own spells
Others=0                ; everybody else's too (an enemy casting an area spell shows where it will land)
EnemiesOnly=0           ; 1 = ONLY what hostile units cast: your own and friendly areas are left out
Precast=1               ; the area of a spell being cast
Channel=1               ; the area of a spell being channeled (Hellfire, Tranquility, ...)
OutlineThickness=0     ; thickness of the bright edge of every shape, in yards: 0.05 to 5; 0 = automatic (a twentieth of the radius, as before)
Triggered=0             ; the areas of the spells that an aura, a missile or a cast triggers (grenades, periodic auras ...)
Instant=0               ; spells with no cast time (Thunder Clap, Arcane Explosion, Cone of Cold ...): an area where they hit, fading away
InstantFadeSeconds=1.5  ; how long the area of an instant or triggered spell takes to fade out once it has gone off: 0.05 to 30, fractions allowed (0.4, 2.5 ...)
Persistent=1            ; areas lying on the ground (the game's dynamic objects)
Cones=1                 ; cone spells
Lines=1                 ; spells whose "cone" is a line (CONE_LINE in the custom attributes file)
LineWidth=4             ; 0.5 .. 40 yards: how wide such a line is
HeightRange=0           ; 0 .. 100 yards above and below an area's center that the ground still takes it; 0 = automatic (half the radius, 4 .. 20)
Opacity=0.65            ; 0.05 .. 1: how strong an indicator is once its cast is done (it fades in from 45% of this)
MaxOpacity=1.0          ; 0.05 .. 1: ceiling for every indicator, applied last; lower it to keep them faint whatever Opacity says
Chain=1                 ; chain spells (Chain Lightning, Chain Heal ...): the circle of the first jump around the target
ChainRadius=12.5        ; 1 .. 60 yards: the jump radius for a chain spell the jump distance file does not list
JumpDistanceFile=WXL\SpellJumpDistance.csv   ; export of the server's spell_jump_distance table (see below)
ConeFile=WXL\SpellCone.csv  ; spell id, cone degrees, one spell per line, no header (see below)
CustomAttributeFile=WXL\SpellsCustomAttribute.csv   ; spell id, attribute mask: cone behind / cone line (see below)
HarmfulColor=FF3C28     ; RRGGBB: areas that hurt (enemy area targets)
HelpfulColor=3CFF5A     ; areas that help (party, raid, ally area targets, area auras)
NeutralColor=FFC83C     ; the rest
ConeAngle=90            ; degrees, for cone spells (the spell data does not carry one)
MinRadius=1.0           ; smaller areas are not drawn
MaxRadius=60            ; bigger ones are drawn at this size
MaxAreas=16             ; at most this many shapes at once
OverrideFile=SpellAreaOverride.csv
```

## What is drawn, and where it comes from

| situation | shape | placed on |
|---|---|---|
| a ground-targeted spell with a cast time (Flamestrike, Mass Dispel) | circle, fading in as the cast goes on | the ground point you aimed at |
| a spell around the caster being cast or channeled (Arcane Explosion, Hellfire) | circle | the caster, following it |
| a spell on a target (area around the target unit) | circle | the target unit |
| a chain spell (Chain Lightning, Chain Heal, Prayer of Mending style jumps ...) | circle of the **first jump**, `ChainRadius` unless the spell data has a radius | the target the cast is aimed at |
| a channel's area aura (Tranquility, Divine Hymn ...) | circle | the caster |
| a cone spell (Cone of Cold, Dragon's Breath ...) | cone, 90 degrees by default | the caster, turning with it |
| a cone spell the custom attributes mark CONE_BACK (a tail sweep) | the same cone, **behind** the caster | the caster, turning with it |
| a cone spell the custom attributes mark CONE_LINE | a **line** as long as the spell's radius, `LineWidth` wide, in front | the caster, turning with it |
| an instant spell that has just gone off, with `Instant=1` (Thunder Clap, Arcane Explosion, Cone of Cold, Fan of Knives ...) | the spell's usual shape (circle, cone or line) at **full** strength that fades out linearly over `InstantFadeSeconds` | where it went off: around or in front of the caster, on the ground point it was aimed at, or around its target; it stays there, it does not follow |
| an aura that casts a spell every few seconds, with `Triggered=1` (periodic trigger auras) | the **triggered** spell's shape | the unit that carries the aura, or its caster for a cone or a circle around the caster, following it for as long as the aura lasts |
| a missile that triggers a spell (grenades, thrown bombs), with `Triggered=1` | the **triggered** spell's circle, filling in like a cast while the missile flies, then at full strength and fading over `InstantFadeSeconds` | where the missile lands: the ground point or the target it was aimed at |
| anything lying on the ground (Blizzard, Consecration, Death and Decay, Rain of Fire, Volley, ground effects of bosses) | circle of its real radius | the game's dynamic object, for as long as it exists |

The size is the spell's own radius from the client data, worked out for the caster's level, and for **your** spells
with your talents' and auras' radius modifiers (the same ones the targeting circle uses). The color says what the spell
does: red for the areas that hit enemies, green for the ones that help friends, amber otherwise; an enemy's dynamic
object is always red.

`EnemiesOnly=1` is for watching what the enemy is about to hit you with: it ignores `Own` and `Others`, and shows the
casts, channels and ground effects of hostile units only (the ones whose reaction toward you is unfriendly or worse;
neutral creatures are not enemies until they turn hostile). Ground effects whose caster is no longer around are left out,
since nobody can say whose they are.

When a dynamic object of a spell exists, it is the one that is drawn (its radius is the truth), so Blizzard shows one
circle, not two.

The chain circle is only shown while the spell is being cast (it needs the target of the cast, or failing that the
caster's current target).

### Where the jump radius comes from

In this order: the spell's row in **WXL\SpellJumpDistance.csv** (the server's own number), then the radius in the spell data
if it has one, then `ChainRadius`. A row in the override file replaces all of it for that spell.

The client cannot ask the server's database, so SpellJumpDistance.csv is an export of the `spell_jump_distance` table of
your `acore_world` database, put in the `WXL` folder beside the client executable. Any of these makes it:

```
mysql -u acore -p acore_world --batch -e "SELECT * FROM spell_jump_distance" > WXL\SpellJumpDistance.csv
```

or "Export table to CSV" in HeidiSQL, MySQL Workbench or DBeaver. Tab, comma or semicolon separated, with the header
line. The columns are found by name: the spell id is the one called `id`, `spell id` or `spell`; the distance is the first
column whose name holds `distance`, else `radius`, else `jump`. If your table names them differently the file is not used
and `WXL.log` (category AOE) lists the headers it saw; rename the headers in the file. A file without a header line
is read as spell id, distance. A spell listed several times (one row per effect) keeps its largest distance. The file is
read again when saved.

## Cone angles: WXL\SpellCone.csv

The spell data has no cone angle, and the server applies cones to some spells whose data only says "area around the
caster". `WXL\SpellCone.csv` (in the `WXL` folder beside the client executable) lists them: one spell per line, **no header**, spell id then degrees. Comma, semicolon or tab
separated, `#` starts a comment, read again when saved:

```
# spell id, degrees
2643,60
12345,90
23456,180
```

A spell that is listed is a cone: its cone target types use the listed angle instead of `ConeAngle`, and its areas
around the caster are drawn as cones of that angle (the radius is still the spell's own). Degrees go above 0 up to 360;
over 180 is the circle minus a wedge behind the caster, and 360 is a full circle. A row in the override file still
beats this file for the same spell. A bad line is skipped and written to `WXL.log` (category AOE).

## Cone direction and shape: WXL\SpellsCustomAttribute.csv

The server marks some spells' cones with custom attributes the client data does not have. `SpellsCustomAttribute.csv`
(in the `WXL` folder beside the client executable) is an export of that column: one spell per line, spell id then the
attribute mask as a decimal number (`0x` hex works too), comma, semicolon or tab separated. The usual
`#spell_id,attributes` header is a comment; read again when saved:

```
#spell_id,attributes
12323,0
26029,4
24340,8
```

Two bits are used, the rest are ignored:

| bit | server name | what is drawn |
|---|---|---|
| `0x2` (2) | `SPELL_ATTR0_CU_CONE_BACK` | the cone is **behind** the caster, like a tail sweep: the same cone turned around |
| `0x4` (4) | `SPELL_ATTR0_CU_CONE_LINE` | the cone is really a **line**: a strip in front of the caster as long as the spell's radius and `LineWidth` yards wide (the cone angle is not used) |

With both bits (6) the line points behind the caster. A spell with one of these bits is treated as a cone even when its
data only says "area around the caster", exactly like a spell listed in `SpellCone.csv`; the listed angle (or
`ConeAngle`) is the cone's opening, and a line ignores it. An area around a destination (ground target) is never turned
into a cone. `Lines=0` hides the line spells, `Cones=0` hides the cone ones. A row in `SpellAreaOverride.csv` still beats
all of this for its spell. A bad line is skipped and written to `WXL.log` (category AOE); when the file is read the
log says how many spells it holds and how many of them are back cones and lines.

## Where a triggered or destination area is centered

A spell's own targets say what its area is around, and the indicator follows them:

| the effect's targets | the area is around |
|---|---|
| an area around the caster (targets 7, 15, 20, 30, 33 ...) or a cone | the caster |
| target 18 (destination = the caster) with a destination area next to it (16, 31, 34 ...) | **the caster**, not the unit the cast was aimed at. Most area spells, and almost every spell the server triggers, are written this way |
| targets 28 / 29 (around the spell's dynamic object) | **the dynamic object** of the spell (an aura that ticks it, such as the one a ground effect puts on a unit, is drawn around that object, not around the unit carrying the aura) |
| any other destination area | the cast's ground point, else the unit it was aimed at; for a periodic aura, the unit that carries it |

A row in `SpellAreaOverride.csv` still decides the shape of a spell it lists.

## Lines, odd cones, hiding a spell: SpellAreaOverride.csv

The spell data has no line shape and no cone angle, and a few spells have an area that is not in it. The two files above
cover what the server's own tables say; this file fixes or adds anything else, spell by spell, and wins over them. It is
beside the client executable and is read again when saved.

```csv
SpellId,Shape,Anchor,Radius,Param,Intent
# a line 40 yards long and 6 wide in front of the caster
12345,line,caster,40,6,harmful
# a cone of 60 degrees and 25 yards
23456,cone,caster,25,60,harmful
# a circle of 8 yards on the aimed point
34567,circle,dest,8,,harmful
# never draw this one
45678,none
```

* `Shape`: `circle`, `cone`, `line` or `none`. `Anchor`: `caster` or `dest` (circles only; cones and lines always start at
  the caster and point where it faces). `Radius`: circle radius, cone radius or line length in yards. `Param`: cone
  angle in degrees, or line width in yards. `Intent`: `harmful`, `helpful` or `neutral` (the color).
* Several rows of one spell all apply. A spell that has rows uses **only** them, not what its data says.
* Comma or semicolon separated; `#` starts a comment; a header line is skipped. A bad line is skipped and written to
  `WXL.log` (category AOE, needs `[Log] Enabled=1`).

## The outline

Every shape is a faint fill with a bright edge. `OutlineThickness` sets how thick that edge is, in **yards**, so a 0.5
outline is half a yard on a 5 yard circle and on a 30 yard one. With 0 (the default) the edge is a twentieth of the
shape's radius, which is thin on small shapes and thick on big ones, as it has always been. The fill is not changed.

* The edge is drawn into the shape's texture (256 pixels across the shape's diameter), so it is never finer than about
  a pixel and a half: on a 40 yard shape anything under about 0.4 yards looks like 0.4. A very thick value on a small
  shape (more than 20 percent of its diameter) is cut short.
* It applies to circles, cones and lines alike, to the instant spell fade, and to anything the override file draws.
* It is hot-reloaded with the ini. Shapes of nearly the same size share a texture, so a few dozen small textures are made
  for the sizes seen; none are made while the value is 0.

## Triggered spells

Many effects do not hurt anyone themselves: they cast another spell. With `Triggered=1` (it is **off** by default, like `Instant`) the indicator looks
those spells up in Spell.dbc (`EffectTriggerSpell`), and when the **triggered** spell has an area (it goes through the same
rules as any other spell: target types, radius, the override and cone files) that area is drawn where it will hit.

| triggered by | the area appears | and goes |
|---|---|---|
| a **periodic trigger aura** (aura types 23 and 226) on a visible unit | while the aura is on the unit, around the unit (around its caster for a cone, a line or a circle centered on the caster) | when the aura is gone |
| a **missile** (effects 32 and 145: grenades, bombs) | when the spell goes off, at the landing point, filling in like a cast while the missile flies | fading over `InstantFadeSeconds` after it lands |
| a **trigger spell** effect (64 and 142) | when the spell goes off, where the triggered spell's own targets are | fading over `InstantFadeSeconds` |

* **Flight time** of a missile is the number the server put in the packet; without one it is the distance to the landing
  point over the spell's `Speed`; without that it lands at once.
* **Whose** areas show is decided by `Own`, `Others` and `EnemiesOnly` against whoever cast the spell (for an aura: who put
  it there). An enemy's triggered area is red whatever the spell's own colour says.
* A triggered spell is shown once: the same shape the triggering spell already draws as an instant spell is not repeated,
  an aura on several units that all draw around its caster draws one, and a fading circle gives way to the ground effect
  when the spell leaves one (Consecration style).
* Only **one level** of triggers is followed: what the triggered spell triggers in turn is not.
* The client only knows the auras the server tells it about. An aura the server keeps hidden (many boss mechanics are) never
  reaches the client, so its area cannot be drawn; a ground effect (dynamic object) is always known.
* A ground effect whose spell works through a periodic trigger takes its colour from the triggered spell.

## Instant spells

A spell with no cast time never shows up as a cast in progress, so the other indicators never see it. With `Instant=1` the
client reports every spell that goes off (the server's spell-go packet) and the ones with **no cast time and no channel**
get an area for `InstantFadeSeconds`: full strength the moment it goes off, then fading out evenly to nothing. The shape is
worked out exactly as for a cast (the override, cone, jump and attribute files all apply), and `Own`, `Others` and
`EnemiesOnly` decide whose spells show.

* Spells the client does not draw a shape for (single target spells, buffs) show nothing, as they do for casts.
* A ground effect that appears with the spell (Consecration ...) replaces the fading circle as soon as it exists, so it is
  not drawn twice. Channels and spells with a cast time are left to `Channel` and `Precast`.
* The areas that are fading count toward `MaxAreas`; the oldest goes first when there are too many.
* Switching `Instant` on does not show spells that went off before.

## Areas over WMO floors

The area is painted on whatever the client's projected-texture pass finds inside a box around its center: terrain, WMO
faces, doodads. Two things decide whether a WMO floor above the terrain gets it:

* **Which WMO faces count.** The stock query skips every WMO face flagged as collision geometry, so a WMO whose floors are
  flagged both render and collision (custom and converted WMOs usually are) is skipped whole. `[Decals]
  WmoCollisionFaces=1` (the default, in `WXLSetup.ini`) lets those faces count; it applies to every ground decal of the
  client, the targeting circle and selection rings included (see `WXL/DecalQuery.h`).
* **How tall the box is.** It reaches `HeightRange` yards above and below the area's center (automatic: half the radius,
  4 to 20 yards). A ground effect whose center the server gives at terrain height can sit that far under a higher floor:
  raise `HeightRange` (try 15 or 25). A taller box also takes in more floors above and below the one you stand on.

## Limits

* Area traps and other game objects (Explosive Trap, a placed totem's radius) are not drawn: they are objects, not
  spell areas. Totem and aura radii that are not channels are not drawn either.
* The cone angle is one value for all cone spells unless a row says otherwise.
* Shapes are placed on the cast's destination as the server sent it; a spell the server aims differently from its
  data will be off.
* Up to `MaxAreas` shapes at once, each costs a terrain query: very many large areas (a raid fight with `Others=1`)
  can cost frames. `MaxRadius` and `MaxAreas` bound it.

## Where the code is

The module (`Engine/Source/WXLModules/SpellArea`, built as `wxl_spellarea.dll`) holds everything that decides what is
drawn; it sees the engine only through the module API (`Engine/Source/WXL/Sdk/wxl_api.h`).

* `SpellAreaShapes.*`: what a spell covers (target types to shapes), the override, jump distance and cone files, the shape
  textures. No engine dependency, tested by `Tools/WxlModuleTest/run_spell_area_tests.sh`.
* `SpellArea.cpp`: reads `[SpellArea]` and the csv files (again whenever they are saved), collects the casts, channels
  and dynamic objects, makes the shapes' textures, and draws. `Module.cpp`: the entry points. The same test script runs
  the module against a stubbed API (`spell_area_module_test.cpp`).
* `Engine/Source/WXLApi/SpellAreaHost.*`: the core's side, the `spellArea*` members of the table. Visible dynamic objects
  and casting units (with their reaction toward the player), a spell's effects with radii worked out for the caster,
  the targets of a cast in progress, an object's position, the texture upload and the projected-texture draw.
* `WorldFrame.cpp`: two `wxl::api::spellareahost::Render` calls beside the client's own cursor shadow; they call the
  renderer the module registered with `setSpellAreaRenderer`, and do nothing without the module.
* `Spell_C.cpp`: `Spell_C_WxlFindPendingCast`, the targets of a cast in progress, and the call in `PendingSpellCast::OnSpellGo`
  that reports a spell going off (`NoteSpellGo`, kept in a ring of 64 for the instant spell fade).

The csv files are read from the client's folder as before (`WXL\SpellJumpDistance.csv`, `WXL\SpellCone.csv`,
`SpellAreaOverride.csv`); the examples next to this file show the format.
