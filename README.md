# Personal notes
This is fork of the decompiled made for personal enjoyment, left open for other people who want to get a 64 bits compiled client with some WXL modules ported in.
Most of the implementation was made by Claude, so there might be a whole lot of spaghetti code in there.

NOTE THAT THIS IS TO BE COMPILED AS 64 BITS

Modules ported and credits:
WXL 1.1 core API and Modern m2 (credits: iThorgrim https://github.com/iThorgrim and the rest of he people working on WXL)
Equip Extension by .CSV (credits: 
							https://github.com/Morfium-G For the original wxl-equipextension module
							https://github.com/Furioz420 For the original .csv implementation
							Myself)
WXL Outlines (credits: https://github.com/WarcraftXL-Extensions/wxl-unit-outline)
Modern Water (credits: https://github.com/jealous-sound/coa-vfog and https://github.com/manatactical/wxl-modern-water)
Physics Engine (credits: https://github.com/TheDaveCalaz/wxl-equip-plus and Domino Physics Engine)

Additional things:
- Spell AoE indicators (WIP, but serviceable at the moment)
- Item model physics through individual .phy2 files (you can extract .phy2 files by using this extractor https://github.com/Atraxian123/WoW_Phys_Extractor_Phy2 on unconverted RETAL models and placing them in thesame folder as the converted model)
- Physics override by item or creature display ID
- configurable items physics through WXLSettings.ini
- underwater physics and buoyancy (while underwater, models behave with different physics constants)
- native .png file compatibility (client will look for .blp, then fallback to .png, then to .tgs before giving a green texture)
- native DXT5 .blp files compatibility for equipment skind instead of just paletted
- automatic construction of "bakednpctextures" for npcs that use character models, but have no linked bakednpctexture in their creaturedisplayidextra.dbc
- outlines for quest targets and tracked npcs and objects
- opt-in for most of the modules
- solved some crashes with overflowing of native m2 models materials that happened on 64 bits client.
- mounts choosing flight animations from the mountflight animation ids before falling back to swimming animations.

all configuration files are in the <root>/WXL folder

KNOWN ISSUES:
- Outlines sometimes screw up the rendering of the image. Just close the client and reopen it and it should be fine.
- Outlines not showing at all when in the setup file the flags for hiding behind walls are set as active.
- Sometimes it crashes when exiting the game. Cause unknown, it might be because I filled my client with gigabites of random patches.

# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.10 or newer (the one that ships with Visual Studio works)
- For Win32 builds only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the build copies it next to the
  executables; the x64 build uses its own decoder instead)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.

## Building

From a Developer PowerShell or any shell with `cmake` on `PATH`:

```powershell
git clone https://github.com/SatyPardus/wowdecomp.git
cd wowdecomp

# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target WowClient --parallel

# Win32 (optional, needs NASM)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build/win32 --config Release --target WowClient --parallel
```

In-source builds are refused, so keep the build directory separate (`build/` above).

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes
`WowClient_crash.log` when the client crashes. `Wow` is the same client without it. You can also turn the
logger off in `WowClient` with `-DWOWCLIENT_CRASH_LOG=OFF`.

### Options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

## Layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
