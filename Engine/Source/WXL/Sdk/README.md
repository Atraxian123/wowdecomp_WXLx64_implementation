# WXL module API (64-bit client)

A **module** is a DLL in `<client folder>\WXL\Modules\` that extends the client through `wxl_api.h` and nothing
else. Modules today (`Engine/Source/WXLModules/`): **physics**, **creature**, **weapon** and **equip**. The equip
module (`wxl_equip.dll`) asks the weapon module for what to attach to a hand through the `"weapon"` service.
The client keeps only the native file reader and the virtual path provider (`Engine/Source/WXLEquip`).

## Why this shape

* **No engine headers in a module.** The client's structs (`CM2Model`, `M2Data`, `M2Array` with its 32-bit
  self-relative offsets) are free to change; a module never has to be rebuilt for it. The core reads them for
  the module (`WXLApi/ModelAccess.cpp`, under SEH) and hands over plain numbers.
* **C ABI only.** No `std::vector`/`std::string`/exception crosses the boundary, so a module may use any
  compiler or CRT. Memory goes back to the side that allocated it (`readFile` -> `freeBuffer`).
* **Events by name.** `subscribe("OnBuildBonePalette", ...)`: the core can add or reorder events without
  breaking a built module.
* **Versioned and grow-only.** `WxlApi` has `abiVersion` and `structSize`; new functions are only ever added
  at its end, and a module checks `WXL_API_HAS(api, member)` before calling one newer than ABI 1.0.

## Loading

`wxl::api::LoadModules()` runs right after `wxl::equip::Init()`, so a module's handlers run **after** the
built-in code's (subscribers run in subscription order). DLLs load in file-name order (`10_x.dll` before
`20_y.dll`): `wxl_equip.dll` loads after `wxl_creature.dll` and before `wxl_physics.dll`, so on
`OnBuildBonePalette` the equip bone remap and off-hand mirror still run before the physics step. For each DLL the loader reads the PE header (a 32-bit DLL is reported as such), loads it, checks
`WxlModuleAbiVersion()`, then calls `WxlModuleInit(api, info)`; a DLL that is not a module, has another ABI,
refuses (returns nonzero) or faults is logged (category `MODULE`) and skipped. Nothing is loaded by a 32-bit
build of the client.

```ini
[Modules]
Enabled=1                     ; 0 = load nothing
Disable=wxl_physics.dll       ; skip these files (';' or ',' separated)
```

## A module in 20 lines

```cpp
#include "wxl_api.h"
WXL_DEFINE_MODULE_ABI()

static const WxlApi* g;
static void OnPalette(void*, const void* args)
{
    void* instance = static_cast<const WxlBuildBonePaletteArgs*>(args)->instance;
    WxlInstanceView v = {}; v.structSize = sizeof(v);
    if (g->readInstance(instance, &v) != WXL_READ_OK) return;     // not loaded / animated yet: normal
    // v.boneCount, v.clockMs, v.sceneMatrix, v.ownMatrix, v.path ... g->readBones(...), g->writePalette(...)
}

WXL_MODULE_API int WxlModuleInit(const WxlApi* api, WxlModuleInfo* info)
{
    if (api->abiVersion != WXL_ABI_VERSION || !WXL_API_HAS(api, writePalette)) return 1;
    g = api;
    std::strncpy(info->name, "mymodule", sizeof(info->name) - 1);
    return api->subscribe(WXL_EVENT_BUILD_BONE_PALETTE, &OnPalette, nullptr);
}
```

CMake (see `Engine/Source/WXLModules/Physics/CMakeLists.txt`): a `SHARED` library, includes `Sdk/` and its own
folder only, output to `bin/Win/WXL/Modules`, built only when `WOWDECOMP_ARCH_64` is set.

## What the table gives a module today

| Area | Members |
|---|---|
| Log | `log`, `logThrottle` (lines go to WXL.log, which is itself opt-in: `[Log] Enabled=1`; with it on, `ModernM2`, `Equip`, `Physics`, `Creature` and `Weapon` in the same section, each defaulting to 1, switch one module's lines off) |
| Events | `subscribe` (by name). Args structs for `OnBuildBonePalette`, `OnUpdate`, `OnWorldEnter/Leave` are in the header |
| Ini | `iniGetString/Int/Bool`, `iniStamp` (WXLExtendedEquipment.ini) |
| Files | `readFile`/`freeBuffer` (archives, then loose files under `Data\`, quiet on a miss), `isVirtualKey` |
| Model instances | `readInstance`, `readBones`, `writePalette` |
| Virtual paths | `vpathPopulateGlobal`, `vpathPopulateGlobalSharedKey`, `vpathPopulate`, `vpathEvictCmo`, `vpathRegisterLazyResolver`, `vpathDecodeGlobalVirtualKey`: the core's table of models patched in memory and served under a minted path (geosets kept, textures replaced, mirrored...) |
| Services | `registerService` / `getService`: a module offers a table of C function pointers by name, another looks it up when it needs it (`"weapon"` is the first, `WxlWeaponService`). Load order does not matter; a missing provider is a NULL answer |
| Virtual files | `registerFileProvider` (the core asks the module for every file open, before the archives; the module copies the bytes into a buffer the core allocates), `registerVirtualKeyQuery` |
| Client files | `fileExists` (can the client open this name? opens and closes, reads nothing) |
| Misc | `workingDirectory` |

The water module (`Engine/Source/WXLModules/Water`) is the module that draws the world's liquids: it renders through the
client's `IDirect3DDevice9` (the one non-C thing it receives, as an opaque COM pointer). The client's state reaches it
through the `"wxl.water.v1"` service the core registers before any module loads (`wxl_water_api.h`: camera, matrices,
day/night light and water colours, liquid types, units standing in liquid, and `setCallbacks` for the five moments of
the world frame it acts at). It starts only with `[Water] Enabled=1`.

The listfile (`Engine/Source/WXLModules/Listfile`) is the fourth module: a file id -> path service (`"wxl.listfile.v1"`, `wxl_listfile_api.h`) other modules call.
The modern M2 reader (`Engine/Source/WXLModules/ModernM2`) is the fifth: the client's M2 loader calls it through `"wxl.modernm2.v1"` (`wxl_modernm2_api.h`) at the load steps of `CM2Shared`, handing it the memory it may use (`WxlM2Alloc`); it names `TXID` textures through the listfile service.

### Moving code that was written inside the client

`wxl_module.hpp` is a header-only C++ layer over the table that keeps the names the in-tree code used
(`wxl::LogCat`, `wxl::storage::ReadNative`, `wxl::events::EventScript::on<>`, the `equipextension::VPath*` and
`WxlIni*` functions). The weapon and creature sources moved to modules by changing their `#include` lines to it
and nothing else; `Module.cpp` calls `wxl::module::Bind(api)` and then the extension's own `Create()`.

The equip module needed engine calls, so the table has an "engine access" section after `fileExists`
(`modelInfo`, `readBoneInfo`, `createModel`, `attachModel`, `copyRemappedBones`, `mirrorPalette`,
`itemDisplayInfo`, `raceClientPrefix`, ... up to `vpathBuildKey`; check `WXL_API_HAS(api, vpathBuildKey)` once).
They take plain numbers and opaque handles: the core (`Engine/Source/WXLApi/EquipHost.cpp`) reads the client's
structures under SEH, so a freed instance gives an error or an empty answer and never a fault in the module.
The equip sources include `EngineApi.hpp` (its own folder) where they used `EngineAccess.h`, and the five
character-model events (`OnItemSlotChange`, `OnItemSlotClear`, `OnHandItemAttached`, `OnM2SkinFinalize`,
`OnM2PerFrameUpdate`) are in `wxl_api.h`.

## Tests

`Tools/WxlModuleTest` holds fake cores that load a module as a shared library through the C ABI (each file starts
with its build line; `stub/windows.h` is just enough of the Windows header to build the modules off Windows):

* `host_test.cpp` (physics): ABI refusals, buffer ownership, virtual-key lookup, the ini switch, shutdown, and a
  plume on a jumping character whose palette the module rewrites. `make_test_data.py` builds its `.phy2`.
* `creature_test.cpp`: the module reads the project's real `WXLCreatureModels.csv` / `WXLCreatureTextures.csv`
  and, through its lazy resolver, asks the core to bake display 33112 with geosets 0/101/201 and its four
  texture layers.
* `weapon_service_test.cpp`: the weapon module registers its service in the core's real registry and the
  equip module's `WeaponService.cpp` gets a baked path back through it.
* `modernm2_test.cpp` (modern M2): loads `wxl_modernm2.dll` as a shared library behind a fake core and drives it through the
  client's adapter (`Model2/ModernM2/ModernM2.cpp`) on synthetic MD21 files and skins; `modernm2_layout_test.cpp` compares the
  module's copy of the M2 record layout with the engine's `M2Data.h`. `run_modernm2_tests.sh` runs both.

What cannot be tested off Windows is `WXLApi/ModelAccess.cpp` (it reads the real `CM2Model`) and the Win32 loader.
