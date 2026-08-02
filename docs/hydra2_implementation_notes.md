# hdMitsuba Hydra 2.0 (Scene Index) Support — Implementation Notes

Validated against conda-forge **OpenUSD 26.05** (HD_API_VERSION 97), Mitsuba 3
@ `3f00b723`, on Linux (CI-equivalent gcc build) — 2026-07-30.

## Context

In OpenUSD 26.05 the scene-index (Hydra 2.0) front-end is the default in
usdview/usdrecord; forcing the legacy path prints a deprecation warning
(`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=0`, "will be removed in a future
release"). hdMitsuba's prims were already partially data-source based
(light/material/instancer/curves read the terminal scene index), but several
paths still depended on legacy `UsdImagingDelegate` behavior.

## Empirically confirmed gaps (before)

A/B renders (usdrecord, scene-index vs legacy pipeline) confirmed:

1. `mitsuba:subdivision_level` silently dropped → coarse geometry
   (RMSE 0.28 vs legacy).
2. Native Mitsuba materials (`outputs:mitsuba:surface`) dropped → fallback
   gray (RMSE 0.04).
3. `mitsuba:sensor:type` / `mitsuba:sensor` dropped → the irradiancemeter
   scene **hung** usdrecord in scene-index mode.
4. `RenderSettings` prims: `mitsuba:*` namespaced settings not forwarded
   (no `GetRenderSettingsNamespaces()`).
5. `render_engine` and the C++ test harness used the deprecated
   `UsdImagingDelegate`.
6. Reactivity gaps found by the fuzzing suite: newly authored
   `treatAsPoint` on lights and cleared material output connections did not
   invalidate.

Root cause for 1–3: the stage scene index is schema-driven; raw custom
attributes are only published if an adapter provides them. The legacy
delegate's `Get()`/`GetCameraParamValue()` read the USD prim directly as a
fallback, which is why these features ever worked.

## Changes

### New: `usd_imaging_mitsuba/` (UsdImaging plugin)

A **keyless** `UsdImagingAPISchemaAdapter`
(`UsdImagingMitsubaAttributesAdapter`) registered with an empty
`apiSchemaName`, so it runs for every prim:

- Publishes authored `mitsuba:*` attributes:
  - Camera prims: overlaid into the `camera` schema container (full attribute
    name as key) → the emulated `GetCameraParamValue()` works unchanged.
  - Other prims: top-level entries of the prim data source, read by the
    delegate through the render index's terminal scene index.
- Invalidation maps attribute edits to locators the emulation translates into
  the dirty bits the delegate actually consumes (`mesh/topology` →
  `DirtyTopology` for re-running `SyncTopology`/`UpdateScene`; `camera` →
  `DirtyParams`).
- Additionally patches over stock-adapter invalidation gaps found by the
  fuzzing suite: light parameters (notably non-`inputs:` attrs like
  `treatAsPoint`, incl. mesh lights) and material `outputs:*` connection
  changes.

Discovered via `PXR_PLUGINPATH_NAME` (wired into `setpath.sh` and the ctest
environment).

### Delegate

- `mesh.cc` / `camera.cc`: custom attributes are read from the terminal
  scene index (every host's input becomes a scene index in current OpenUSD,
  so the terminal index always exists); an unauthored attribute is an empty
  value.
- `material.cc`: per-terminal render-context resolution — start from the
  universal network, overlay `mitsuba`-context nodes/terminals (a material can
  mix a universal preview surface with `outputs:mitsuba:displacement`). An
  empty/removed network now syncs an empty `MaterialSpec` so stale materials
  are replaced by the fallback BSDF.
- `render_delegate`: `GetRenderSettingsNamespaces()` → `{"mitsuba"}`, so
  `HdsiRenderSettingsFilteringSceneIndex` forwards `mitsuba:*` settings from
  RenderSettings prims in scene-index mode.

### render_engine

`UsdImagingCreateSceneIndices` chain inserted into the render index
(replacing `UsdImagingDelegate`), plus:

- `HdsiImplicitSurfaceSceneIndex` (implicit → mesh; the delegate only supports
  mesh/basisCurves — the legacy adapters used to do this),
- `HdsiLegacyDisplayStyleOverrideSceneIndex` (refine-level fallback),
- `ApplyPendingUpdates()` + `SetTime()` per render (stage edits between
  renders).

`HDMITSUBA_ENGINE_USE_SCENE_INDEX=false` selects the deprecated legacy
front-end for A/B comparisons.

### Tests

`tests/test_util.h` gains `CreateSceneIndexTestHarness`: stage → scene index
chain → `InsertSceneIndex` → sync via a minimal task
(collection + `geometry` render tag; rprims only sync for render tags
declared by tasks). The camera and mesh C++ tests run on it, including an
end-to-end invalidation test (edit `mitsuba:subdivision_level` on the stage →
adapter invalidation → re-subdivision).

Note: an externally constructed `HdSceneIndexAdapterSceneDelegate` is *not* a
usable harness — as the render index's internal emulation class, its inserts
bypass the emulation scene index, so the terminal scene index stays empty.

## Verification (all on the scene-index pipeline unless noted)

| Check | Result |
|---|---|
| ctest (scene-index C++ harness) | 4/4 |
| pytest | 118 passed / 7 skipped (== legacy baseline) |
| pytest, legacy fallback (`HDMITSUBA_ENGINE_USE_SCENE_INDEX=false`) | 118 / 7 |
| Fuzzing suite | 188/188 |
| usdrecord A/B scene-index vs legacy: subdiv | RMSE 0.0023 (sampling noise; was 0.28) |
| … native material (checkerboard) | RMSE 0 (was 0.04) |
| … mesh light | RMSE 0 |
| … irradiancemeter incl. explicit camera | RMSE 0 (was a hang) |
| RenderSettings prim `mitsuba:integrator:type` via usdrecord | applied (direct vs path clearly differs) |
| usdview interactive (Xvfb), materials/CornellBox/Kitchen_set | renders, "Hydra: Mitsuba" |

## Notes / follow-ups

- usdview's "Enable Default Camera Light" / "Enable Default Dome Light"
  toggles reach the delegate as real light prims (`distantLight` /
  `domeLight`) under the application's private `/_UsdImaging_*` root, via
  `HdxTaskController` lighting emulation. Both are translated literally; note
  that the GL-convention camera headlight adds unauthored energy and will
  brighten/flatten physically-based renders — prefer the default dome light
  (a constant Mitsuba environment) for inspecting unlit assets. The
  overexposed look of some test scenes in usdview/usdrecord
  (e.g. the Cornell box) is the raw HDR radiance shown without
  exposure/tonemapping and is identical in both pipelines; the pytest image
  outputs (written through `mi.Bitmap` sRGB conversion) show the expected
  exposure.
- The invalidation gaps patched in the adapter (light `treatAsPoint`,
  material `outputs:*` connection edits) look like stock UsdImaging adapter
  limitations in 26.05 and may deserve an upstream issue.
- Possible next step for "native" Hydra 2.0: consuming the terminal scene
  index via `HdsiPrimManagingSceneIndexObserver` instead of rprims/emulation;
  not required for correct usdview behavior.

