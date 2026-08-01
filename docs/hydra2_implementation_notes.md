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
- `mesh.cc` additionally reads visibility, the prim transform and the bound
  material id schema-natively (`HdVisibilitySchema`, `HdXformSchema`,
  `HdMaterialBindingsSchema` on the terminal scene index) instead of
  `GetVisible()`/`GetTransform()`/`GetMaterialId()`.
- `mesh.cc` also reads topology, subdivision tags, the refine level and
  primvars schema-natively: `HdMeshSchema`/`HdMeshTopologySchema` (with geom
  subsets gathered from `geomSubset` *child prims* of the mesh — their Hydra
  2.0 representation — including per-subset material bindings),
  `HdSubdivisionTagsSchema`, `HdLegacyDisplayStyleSchema`, and
  `HdPrimvarsSchema`/`HdPrimvarSchema` (descriptors in a single pass instead
  of one query per interpolation; values flattened by the schema). Absent
  schemas resolve to explicit defaults (visible, identity transform, no
  binding, no subdiv tags, refine level 0, no primvars, empty primvar
  values and indices) rather than legacy scene-delegate calls — an
  instrumented audit (every legacy fallback made fatal) across ctest, the
  full pytest suite, fuzzing and the usdview compliance driver showed the
  only reachable "fallbacks" were the miss paths for genuinely absent
  data.
- Material edits that only change parameter *values* (colors, roughness —
  same nodes, connections, terminals and file/string parameters) now update
  the existing Mitsuba BSDF **in place** through its traversal interface: a
  freshly translated twin supplies the values (identical structure ⇒
  identical traversal layout), children get `parameters_changed()` before
  parents, and any parameter that cannot be copied faithfully falls back to
  a full rebuild — an in-place update is never silently wrong. The pre-Hydra
  2.0 scaffolding for this (the `dirty_bits` path in the scene manager and
  an empty `UpdateMaterialInPlace` stub) is now live. Verified equivalent to
  a from-scratch render (max abs diff ~4e-7) with kernel freezing both off
  and on — frozen kernels stay valid across interactive material tweaks.
  Two guards found by the fuzzing suite: value changes touching the 0/1
  boundaries (or flipping bools/ints, or crossing an all-zero color) count
  as structural, because Mitsuba plugins bake feature gates at construction
  from exactly those values (`has_metallic`, opacity handling, ...) and the
  gates are invisible to traversal; and `parameters_changed()` receives each
  object's full parameter-name list as keys, since several plugins (e.g. the
  principled BSDF's eta/specular derivation) skip recomputation for keys not
  named.
- Texture color spaces are driven by the network schema's per-parameter
  `colorSpace` field (UsdImaging folds the UsdUVTexture `sourceColorSpace`
  input into it; the parameter itself no longer appears in scene-index
  networks, so the old parameter-based read silently degraded to input-name
  heuristics for everything). An explicit `raw`/`sRGB` from the schema now
  overrides the heuristic when loading bitmaps.
- Ext computations (UsdSkel skinning) are no longer evaluated in the
  delegate: `scene_index_plugin.cc` registers
  `HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin`
  (`HdSiExtComputationPrimvarPruningSceneIndex`) for the Mitsuba renderer via
  `HdSceneIndexPluginRegistry`, and the render index appends it automatically
  for *any* scene-index host — usdview, usdrecord, the render_engine — with
  no host-side wiring. Skinned points arrive as plain `points` primvars, so
  the `HdExtComputationUtils` block in `SyncPrimvars` is gone. This is the
  textbook Hydra 2.0 win: a cross-cutting feature added as a filter over the
  scene, not as per-prim delegate code. The delegate also no longer
  advertises or creates `extComputation` sprims — the filter consumes those
  prims before they reach the backend. A regression test scrubs time on
  `skinned_mesh.usda` with a single engine to pin down invalidation through
  the filter.
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

The deprecated `UsdImagingDelegate` front-end (and its
`HDMITSUBA_ENGINE_USE_SCENE_INDEX` escape hatch) has since been **removed**
from `render_engine` and the C++ test harness — the scene-index chain is the
only front-end. The A/B numbers below were gathered while both paths still
existed and are kept as the historical record of the switch.

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
| pytest, legacy fallback (before its removal) | 118 / 7 |
| Fuzzing suite | 188/188 |
| usdrecord A/B scene-index vs legacy: subdiv | RMSE 0.0023 (sampling noise; was 0.28) |
| … native material (checkerboard) | RMSE 0 (was 0.04) |
| … mesh light | RMSE 0 |
| … irradiancemeter incl. explicit camera | RMSE 0 (was a hang) |
| RenderSettings prim `mitsuba:integrator:type` via usdrecord | applied (direct vs path clearly differs) |
| usdview interactive (Xvfb), materials/CornellBox/Kitchen_set | renders, "Hydra: Mitsuba" |

### Variant switches no longer use the legacy dirtying API

A mid-session `mitsuba:variant` change (e.g. usdview restoring a persisted
renderer setting) recreates the templated scene manager. The old path then
forced a global re-sync via `HdChangeTracker::MarkAllRprimsDirty` /
`MarkSprimDirty` / `MarkBprimDirty` — front-end legacy API that Hydra 2.0
refuses for scene-index-fed prims ("Calling method on HdChangeTracker that
requires emulation") and silently no-ops, leaving the new scene manager
empty ("No sensor specified"). The prim specs are Mitsuba-variant
independent, so the new manager is now seeded directly from the previous
manager's cached specs (`SceneManager::SeedSpecsFrom`) and rebuilds the
scene without dirtying a single Hydra prim. Verified: a variant switch
renders identically (`usdview_compat_test.py`), in both the default and the
observer-renderer UsdImagingGL wirings.

### Application-level (usdview wiring) compliance tests

`tests/usdview_compat_driver.py` drives hdMitsuba through the same
UsdImagingGL + HdxTaskController stack usdview uses (headless via
xvfb + a usdrecord-style offscreen GL context):
scene camera, free camera via `SetCameraState`, usdview's default camera
light via `SetLightingState`, every advertised AOV, and a mid-session
variant switch — failing on any Tf error. `tests/usdview_compat_test.py`
runs it in both engine wirings and adds an image-equivalence test for the
variant switch. This is the layer where "works in the test suite, breaks in
usdview" bugs live: the unit/fuzzing suites drive the delegate through the
render_engine, which never pushes renderer settings mid-session and never
uses task-controller cameras or lighting.

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

