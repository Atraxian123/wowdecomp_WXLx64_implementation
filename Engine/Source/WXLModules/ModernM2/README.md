> **The modern M2 reader is a WXL module**: `wxl_modernm2.dll`, built from this folder, loaded from `WXL\Modules\` by the
> 64-bit client. It sees the client only through `Engine/Source/WXL/Sdk/wxl_api.h`; the client calls it through the
> service `wxl.modernm2.v1` (`Engine/Source/WXL/Sdk/wxl_modernm2_api.h`). The client's side is one file,
> `Engine/Source/Model2/ModernM2/ModernM2.cpp`.
> Test: `Tools/WxlModuleTest/modernm2_test.cpp` (the module through the C ABI) and `modernm2_layout_test.cpp`;
> `run_modernm2_tests.sh` builds and runs both.

# Modern M2: MD21 models for the 3.3.5a client

Reads models from later builds (an `MD21` container, inner version 272-274) and hands the client's own loader the flat
`MD20` body it understands. The loader's walk (`M2Init`) is the stock one; this module is everything around it.

| Step (client) | What the module does |
| --- | --- |
| `CM2Shared::FinishLoading`, before the walk: `prepareLoad` | demuxes the container, seats the body in a block the client allocates (with spare room after it), applies the in-place field deltas, rewrites particle / camera records that grew to the client's shape, files the later-era flight sequences under the client's flight ids, harvests the `TXID` file ids |
| after the walk: `finishWalk` | clamps ribbon texture / material indices and camera lookup sentinels |
| `CM2Shared::Initialize`: `textureName` | names a hardcoded texture the file leaves unnamed by its `TXID` file id, through the listfile module (`wxl.listfile.v1`); an id the listfile does not know comes back as the listfile's not-found name, so the client shows its own missing texture |
| `CM2Shared::FinishLoadingSkinProfile`: `finalizeSkin` | the bone-budget split (75 bones per draw, for every model), the material / texture-unit contract rebuild a modern model needs, `boneInfluences` fix, full 32-bit index starts for models that use the extended encoding |
| external animation files: `unwrapAnim` | slides the payload out of the one-chunk `AFM2` wrapper |

A split-skeleton model (`SKID`, a `.skel` file) is refused like an absent file: there is no skeleton splice.

## What lives where

* **Module:** every byte-level decision above. It carries its own copy of the client's record layout (`M2Records.hpp`,
  no engine header may be included) and keeps **no state between calls**.
* **Client (`ModernM2.cpp`):** the per-model `State` (TXID table, spare room used, full index starts), and the memory.
  Every block the module needs comes through `WxlM2Alloc`, from the client's own allocators, so a block is freed by the
  routine that allocated it (`SIMD::FreeA16`, `OsFreeNear`) and no pointer crosses the DLL boundary the wrong way.
* **Shared, engine-free:** `Engine/Source/Model2/ModernM2/FlightAnim.h` (constants and a pure decision function; the
  module remaps the source flight ids with it, the game layer resolves them with it). A unit in flight plays the first of
  MOUNT_FLIGHT_* (548 / 556 / 552 / 554 / 562, native or modern), ANIM_FLY_SWIM_* (0x10E-0x112) and the swim set that its
  model has.

`M2Records.hpp` is a copy of `Engine/Source/Model2/M2Data.h`. The format is the old client's and does not move, but a copy
can drift: `modernm2_layout_test.cpp` compares every size and field offset with the engine's header (it is generated from
it; regenerate the lists when the engine header gains a record).

## Without the module

The client never needs it to start. `wxl_modernm2.dll` missing (or in `[Modules] Disable=`) means:

* a stock model loads exactly as before;
* a modern model is refused (`Corrupt model data`), once in the log under `M2`:
  `... is a modern model but the module (wxl.modernm2.v1) is not loaded`;
* the bone-budget split and the other skin rebuilds are not run. The one fix every stock model always got
  (`boneInfluences == 0` lifted to 1) and the shadow-pass gate stay in the client.

A 32-bit build of the client builds no modules and so reads stock models only.

## Load order

The client looks the service up when a model loads, not at start-up, so load order against other modules does not
matter. `textureName` needs `wxl_listfile.dll` loaded to give real paths; without it a `TXID` texture stays the solid white
placeholder and one line says so.

## Log

Category `M2` (switch: `[Log] ModernM2=0` in `WXLSetup.ini`, with `[Log] Enabled=1`): one line per modern
model read, with the version, the `TXID` count, which source-only chunks were skipped, and which records were
normalized; one line per refusal with the reason.

## Limits

An inner version outside 272-274 is refused. At most 128 `TXID` entries are kept per model. Everything past the
module's own bounds checks (a hostile count, an offset outside the body) refuses the model instead of reading on.
