# Out-of-tree hdEmbree build

Several tests compare hdMitsuba against **HdEmbree**, USD's reference
path-tracing render delegate (`camera_test.test_compare_to_hdembree` and
the `HdEmbreeRendererPlugin` half of the parametrized `engine_test` /
`render_test` cases). Prebuilt USD distributions — the conda-forge
`openusd` package included — are built without the Embree plugin, so those
tests report `HdEmbreeRendererPlugin not available` and skip.

This kit builds just the plugin from the matching OpenUSD sources against
your installed USD SDK and drops it next to hdStorm, activating the
skipped tests (they detect the plugin dynamically — no test changes
needed). As a bonus, Embree becomes selectable as a renderer in usdview.

## Prerequisites

- Network access (fetches ~15 source files from the OpenUSD GitHub repo).
- Embree 4: `brew install embree` (macOS) or the `embree` conda-forge /
  distro package.
- The usual build tools (cmake, ninja) and the conda env that provides
  USD (`pxrConfig.cmake` at `$CONDA_PREFIX`).

## Build and install

```sh
cd tools/hdembree
./fetch_hdembree.sh   # detects the installed USD version, e.g. v26.05
cmake -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX"
cmake --build build
cmake --install build
```

If embree lives outside the default search paths, add its prefix to
`CMAKE_PREFIX_PATH` (e.g. `"$CONDA_PREFIX;$(brew --prefix embree)"`).

## Verify

```sh
python -c "from pxr import UsdImagingGL; print(UsdImagingGL.Engine.GetRendererPlugins())"
```

should now list `HdEmbreeRendererPlugin`; the previously skipped tests run
on the next `pytest` invocation.

## Uninstall

```sh
rm "$CONDA_PREFIX"/plugin/usd/hdEmbree.*
rm -r "$CONDA_PREFIX"/plugin/usd/hdEmbree
```

## Notes

- `fetch_hdembree.sh` derives the OpenUSD tag from the installed USD
  (`Usd.GetVersion()`); pass a tag explicitly to override, e.g.
  `./fetch_hdembree.sh v26.05`.
- The fetched sources stay untracked (`src/` is gitignored) — they are
  Pixar's code, pulled fresh per machine rather than vendored here.
- Installing into `$CONDA_PREFIX` is the zero-configuration path. To keep
  the env pristine instead, install anywhere else and point
  `PXR_PLUGINPATH_NAME` at `<prefix>/plugin/usd/hdEmbree/resources`.
