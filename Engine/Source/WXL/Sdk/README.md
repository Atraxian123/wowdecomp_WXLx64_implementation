# WXL module API (64-bit client)

A **module** is a DLL in `<client folder>\WXL\Modules\` that extends the client through `wxl_api.h` and nothing
else. The built-in extensions (equip, weapon, creature) are still compiled into the client; the physics is the
first extension that is a module (`Engine/Source/WXLModules/Physics`).

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

The equip, weapon and creature extensions need more (item/creature display records, attaching models, texture
creation); those calls are the next additions to the table, and their sources move to modules the same way the
physics did: engine access through the table, sources that include only `wxl_api.h`.

## Tests

`Tools/WxlModuleTest/host_test.cpp` is a fake core: it builds a module as a shared library, loads it through
the C ABI, fires `OnBuildBonePalette` for a plume on a jumping character and checks what the module wrote back
(ABI refusals, buffer ownership, virtual-key lookup, the ini switch, shutdown). `make_test_data.py` builds its
`.phy2` with the repo's own converter. The part that cannot be tested off Windows is `WXLApi/ModelAccess.cpp`
(it reads the real `CM2Model`) and the Win32 loader.
