# wxl_water

The standalone `wxl-modern-water` module (itself built on CoAVolFog's renderer) as a WXL module, `wxl_water.dll`,
loaded from `<client folder>\WXL\Modules\` by the 64-bit client: lakes, rivers, the sea and indoor pools shaded with
moving FFT waves, refraction, foam, sun highlights and scene reflections. Magma and slime keep the client's look.
Off by default.

## Switching it on

`WXLExtendedEquipment.ini`, beside the client executable:

```ini
[Water]
Enabled=1
```

With it off (or absent) the DLL refuses to load, logs why under category `WATER`, and the client draws its own water.

The module's own settings (`WaterQuality`, `WaterWaves`, ripples, ...) are in `wxl-modern-water.ini`, which with
`waterdata.bin` goes in `WXL\Modules\wxl_water\`; the build copies both there. `waterdata.bin` is binary and not part
of the patch: copy `data/waterdata.bin` from the standalone module into `Engine/Source/WXLModules/Water/data/`.
Without it the module refuses to load. The log is `WXL\Modules\wxl_water\wxl-modern-water.log`.

`Enable`, `EngineHooks`, `Overlay` and `OverlayKey` in that ini belong to the standalone module's loader and settings
window and have no effect here; everything else works and is re-read within a second while the game runs.

## How it is built

A module includes no engine header (see `Engine/Source/WXL/Sdk/README.md`). The renderer, FFT, spectrum, ripple and
device-wrapper code is the standalone module's, unchanged. What read or patched the 3.3.5a binary is replaced:

| Standalone | Here |
|---|---|
| `engine.cpp`, `engine_actors.cpp`: absolute-address reads | the core reads the client's own objects (`Engine/Source/WXLApi/WaterHost.cpp`) and offers them as the C service `"wxl.water.v1"` (`Sdk/wxl_water_api.h`); `bridge.cpp` is the module's end of it; `engine_decomp.cpp` and `engine_actors_decomp.cpp` keep the module's logic on top |
| `hooks.cpp`: detours at the opaque pass, the liquid pass and the two water materials | calls in the client at the same four places (`Engine/Source/WXL/WaterHooks.h`) that the module claims with the service's `setCallbacks`; `water_driver.cpp` is the water half of `hooks.cpp` |
| device wrapped at creation | the live device is adopted (the module already supported it) and un-adopted around the client's `Reset`, through the `OnDeviceLost` / `OnDeviceReset` events |
| WXL overlay settings window | none: `overlay_stub.cpp` |
| client ripple sprite switch guarded by code bytes | the service's `clientRippleGate`, a pointer to `CWorld::s_waterRipples` |

The only thing that crosses to the module that is not plain C is the `IDirect3DDevice9*`, as an opaque pointer to a COM
object; the module links the system's d3d9 for it. The fog and grading renderers ride along compiled but are never
driven: nothing calls them.

64-bit only, like every module. `msaa_depth.cpp` loads `nvapi64.dll` for the multisampled depth copy.

## Credits

See `LICENSE` (GPLv3). Water model, shaders and renderer: wxl-modern-water and CoAVolFog.
