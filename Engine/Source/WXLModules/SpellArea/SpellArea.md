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
Persistent=1            ; areas lying on the ground (the game's dynamic objects)
Cones=1                 ; cone spells
Opacity=0.65            ; 0.05 .. 1: how strong an indicator is once its cast is done (it fades in from 45% of this)
MaxOpacity=1.0          ; 0.05 .. 1: ceiling for every indicator, applied last; lower it to keep them faint whatever Opacity says
Chain=1                 ; chain spells (Chain Lightning, Chain Heal ...): the circle of the first jump around the target
ChainRadius=12.5        ; 1 .. 60 yards: the jump radius for a chain spell the jump distance file does not list
JumpDistanceFile=WXL\SpellJumpDistance.csv   ; export of the server's spell_jump_distance table (see below)
ConeFile=WXL\SpellCone.csv  ; spell id, cone degrees, one spell per line, no header (see below)
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

## Lines, odd cones, hiding a spell: SpellAreaOverride.csv

The spell data has no line shape and no cone angle, and a few spells have an area that is not in it. A file beside the
client executable fixes that, spell by spell. It is read again when saved.

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
* `Spell_C.cpp`: `Spell_C_WxlFindPendingCast`, the targets of a cast in progress.

The csv files are read from the client's folder as before (`WXL\SpellJumpDistance.csv`, `WXL\SpellCone.csv`,
`SpellAreaOverride.csv`); the examples next to this file show the format.
