# WXL module API (64-bit client)

A **module** is a DLL in `<client folder>\WXL\Modules\` that extends the client through `wxl_api.h` and nothing
else. Modules today (`Engine/Source/WXLModules/`): **physics**, **creature** and **weapon**. The equip extension
is still compiled into the client (its engine calls are the next additions to the table); it asks the weapon
module for what to attach to a hand through the `"weapon"` service.

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
built-in extensions' (subscribers run in subscription order). DLLs load in file-name order (`10_x.dll` before
`20_y.dll`). For each DLL the loader reads the PE header (a 32-bit DLL is reported as such), loads it, checks
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
| Log | `log`, `logThrottle` (lines go to WXL.log, which is itself opt-in: `[Log] Enabled=1`) |
| Events | `subscribe` (by name). Args structs for `OnBuildBonePalette`, `OnUpdate`, `OnWorldEnter/Leave` are in the header |
| Ini | `iniGetString/Int/Bool`, `iniStamp` (WXLExtendedEquipment.ini) |
| Files | `readFile`/`freeBuffer` (archives, then loose files under `Data\`, quiet on a miss), `isVirtualKey` |
| Model instances | `readInstance`, `readBones`, `writePalette` |
| Virtual paths | `vpathPopulateGlobal`, `vpathPopulateGlobalSharedKey`, `vpathPopulate`, `vpathEvictCmo`, `vpathRegisterLazyResolver`, `vpathDecodeGlobalVirtualKey`: the core's table of models patched in memory and served under a minted path (geosets kept, textures replaced, mirrored...) |
| Services | `registerService` / `getService`: a module offers a table of C function pointers by name, another looks it up when it needs it (`"weapon"` is the first, `WxlWeaponService`). Load order does not matter; a missing provider is a NULL answer |
| Virtual files | `registerFileProvider` (the core asks the module for every file open, before the archives; the module copies the bytes into a buffer the core allocates), `registerVirtualKeyQuery` |
| Client files | `fileExists` (can the client open this name? opens and closes, reads nothing) |
| Misc | `workingDirectory` |

The listfile (`Engine/Source/WXLModules/Listfile`) is the fourth module: a file id -> path service (`"wxl.listfile.v1"`, `wxl_listfile_api.h`) other modules call.

### Moving code that was written inside the client

`wxl_module.hpp` is a header-only C++ layer over the table that keeps the names the in-tree code used
(`wxl::LogCat`, `wxl::storage::ReadNative`, `wxl::events::EventScript::on<>`, the `equipextension::VPath*` and
`WxlIni*` functions). The weapon and creature sources moved to modules by changing their `#include` lines to it
and nothing else; `Module.cpp` calls `wxl::module::Bind(api)` and then the extension's own `Create()`.

The equip extension still needs engine calls the table does not have yet (`CreateModel`, attach to a parent,
texture creation, the item/race display records): those are the next additions, behind the same kind of C
functions, and `EquipExtension.cpp` then moves the same way.

## Tests

`Tools/WxlModuleTest` holds fake cores that load a module as a shared library through the C ABI (each file starts
with its build line; `stub/windows.h` is just enough of the Windows header to build the modules off Windows):

* `host_test.cpp` (physics): ABI refusals, buffer ownership, virtual-key lookup, the ini switch, shutdown, and a
  plume on a jumping character whose palette the module rewrites. `make_test_data.py` builds its `.phy2`.
* `creature_test.cpp`: the module reads the project's real `WXLCreatureModels.csv` / `WXLCreatureTextures.csv`
  and, through its lazy resolver, asks the core to bake display 33112 with geosets 0/101/201 and its four
  texture layers.
* `weapon_service_test.cpp`: the weapon module registers its service in the core's real registry and the
  in-tree `WeaponService.cpp` (the equip side) gets a baked path back through it.

What cannot be tested off Windows is `WXLApi/ModelAccess.cpp` (it reads the real `CM2Model`) and the Win32 loader.
