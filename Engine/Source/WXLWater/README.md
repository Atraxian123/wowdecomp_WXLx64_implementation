# WXLWater

The standalone `wxl-modern-water` module (itself built on CoAVolFog's renderer) running inside the client:
lakes, rivers, the sea and indoor pools shaded with moving FFT waves, refraction, foam, sun highlights and scene
reflections. Magma and slime keep the client's look. Off by default.

## Switching it on

`WXLExtendedEquipment.ini`, beside the client executable:

```ini
[Water]
Enabled=1
```

The module's own settings (`WaterQuality`, `WaterWaves`, ripples, ...) are in `wxl-modern-water.ini`, which with
`waterdata.bin` goes into a `WXLWater` folder beside the executable. Set `-DWXLWATER_DEPLOY_DIR=<that folder>` and the
build copies them there. `waterdata.bin` is binary and not part of the patch: copy `data/waterdata.bin` from the
standalone module into `Engine/Source/WXLWater/data/`. Without it the water stays off and the log says so.
The log is `WXLWater\wxl-modern-water.log`.

`Enable`, `EngineHooks`, `Overlay` and `OverlayKey` in that ini belong to the standalone module's loader and settings
window and have no effect here; everything else works and is re-read within a second while the game runs.

## What changed from the standalone module

The renderer, FFT, spectrum, ripple and device-wrapper code is carried over unchanged. Only the parts that read or
patched the 3.3.5a binary were replaced:

| Standalone | Here |
|---|---|
| `engine.cpp`, `engine_actors.cpp`: absolute-address reads | `bridge.cpp`: the client's own named objects; `engine_decomp.cpp` and `engine_actors_decomp.cpp` keep the module's logic on top |
| `hooks.cpp`: detours at the opaque pass, the liquid pass and the two water materials | calls in the client at the same four places, through `Engine/Source/WXL/WaterHooks.h`; `water_driver.cpp` is the water half of `hooks.cpp` |
| device wrapped at creation | the live device is adopted (as the module already supported) and un-adopted around the client's `Reset` via `OnDeviceLost` / `OnDeviceReset` |
| WXL overlay settings window | none: `overlay_stub.cpp` |
| client ripple sprite switch guarded by code bytes | `CWorld::s_waterRipples`, no guard needed |

The fog and grading renderers ride along compiled but are never driven: nothing calls them.

## Credits

See `LICENSE` (GPLv3). Water model, shaders and renderer: wxl-modern-water and CoAVolFog.
