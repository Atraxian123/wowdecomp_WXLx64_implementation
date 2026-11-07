# Spell area indicator

Draws the ground area of spells as colored, terrain-hugging shapes: while a spell is **being cast** (the precast),
while it is **channeled**, and for as long as it **lies on the ground** (Blizzard, Flamestrike, Consecration, Death and
Decay, Rain of Fire ...). Circles, cones and lines. Off by default; part of the client, no module DLL.

It uses the client's own projected-texture pass, the one that paints the targeting circle under the cursor, so the
shapes follow terrain, WMOs and doodads and show over water. The client's circle while you aim is untouched.

## Turning it on

```ini
[SpellArea]
Enabled=1
```

The ini is looked at once a second: save it and the indicator switches on or off, and picks up the other keys, without
restarting.

## Keys (all optional)

```ini
[SpellArea]
Enabled=1
Own=1                   ; your own spells
Others=0                ; everybody else's too (an enemy casting an area spell shows where it will land)
Precast=1               ; the area of a spell being cast
Channel=1               ; the area of a spell being channeled (Hellfire, Tranquility, ...)
Persistent=1            ; areas lying on the ground (the game's dynamic objects)
Cones=1                 ; cone spells
Opacity=0.65            ; 0.05 .. 1
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
| a channel's area aura (Tranquility, Divine Hymn ...) | circle | the caster |
| a cone spell (Cone of Cold, Dragon's Breath ...) | cone, 90 degrees by default | the caster, turning with it |
| anything lying on the ground (Blizzard, Consecration, Death and Decay, Rain of Fire, Volley, ground effects of bosses) | circle of its real radius | the game's dynamic object, for as long as it exists |

The size is the spell's own radius from the client data, worked out for the caster's level, and for **your** spells
with your talents' and auras' radius modifiers (the same ones the targeting circle uses). The color says what the spell
does: red for the areas that hit enemies, green for the ones that help friends, amber otherwise; an enemy's dynamic
object is always red.

When a dynamic object of a spell exists, it is the one that is drawn (its radius is the truth), so Blizzard shows one
circle, not two.

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

* `Engine/Source/WXL/SpellAreaShapes.*`: what a spell covers (target types to shapes), the override file, the shape
  textures. No engine dependency, tested by `Tools/WxlModuleTest/run_spell_area_tests.sh`.
* `Engine/Source/WXLApi/SpellArea.*`: collects the casts, channels and dynamic objects, and draws.
* `WorldFrame.cpp`: two `wxl::spellarea::Render` calls beside the client's own cursor shadow.
* `Spell_C.cpp`: `Spell_C_WxlFindPendingCast`, the targets of a cast in progress.
