# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Measures interactive material-edit latency: native vs emulated backend.

Builds a grid of meshes, each with its own bound UsdPreviewSurface material,
then repeatedly edits every material's diffuseColor (one interactive "slider
tick" touching all of them) and measures the edit->rendered latency. Run
once per backend mode:

  python material_edit_benchmark.py --meshes 512 --edits 30
  HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX=1 python material_edit_benchmark.py ...

The interesting number is the Hydra-side update cost: the time from the USD
edit until the delegate has re-translated every dirtied material. On the
native backend that is one batch of observer notifications translated
directly; on the emulated path every dirtied material sprim runs through
the render index's dirty-bit mapping and per-prim Sync machinery. Rendering
cost is identical in both modes, so the benchmark renders at a tiny
resolution and low spp to keep the render term small, and separately
reports a sync-only estimate obtained by subtracting the steady-state
render time measured with no pending edits.
"""

import argparse
import colorsys
import json
import os
import statistics
import sys
import time

import mitsuba as mi

mi.set_variant('cuda_ad_rgb', 'llvm_ad_rgb')

from pxr import Sdf, Usd, UsdGeom, UsdLux, UsdShade

import usd_render
from hdmitsuba.tests import test_helpers


def build_stage(mesh_count: int) -> tuple[Usd.Stage, list[UsdShade.Shader]]:
  stage = Usd.Stage.CreateInMemory()
  UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
  UsdGeom.Xform.Define(stage, '/root')

  side = max(1, int(round(mesh_count ** 0.5)))
  camera = UsdGeom.Camera.Define(stage, '/root/camera')
  UsdGeom.XformCommonAPI(camera.GetPrim()).SetTranslate(
      (side * 1.25, side * 1.25, side * 3.2)
  )
  UsdLux.DomeLight.Define(stage, '/root/dome')

  shaders = []
  for i in range(mesh_count):
    mesh = UsdGeom.Mesh.Define(stage, f'/root/m{i}')
    x = (i % side) * 2.5
    y = (i // side) * 2.5
    mesh.CreatePointsAttr(
        [(x - 1, y - 1, 1), (x + 1, y - 1, 1), (x - 1, y + 1, 1),
         (x + 1, y + 1, 1), (x - 1, y + 1, -1), (x + 1, y + 1, -1),
         (x - 1, y - 1, -1), (x + 1, y - 1, -1)]
    )
    mesh.CreateFaceVertexCountsAttr([4] * 6)
    mesh.CreateFaceVertexIndicesAttr(
        [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3,
         6, 0, 2, 4]
    )
    material = UsdShade.Material.Define(stage, f'/root/mats/mat{i}')
    shader = UsdShade.Shader.Define(stage, f'/root/mats/mat{i}/surface')
    shader.CreateIdAttr('UsdPreviewSurface')
    shader.CreateInput('diffuseColor', Sdf.ValueTypeNames.Color3f).Set(
        (0.5, 0.5, 0.5)
    )
    shader.CreateInput('roughness', Sdf.ValueTypeNames.Float).Set(0.5)
    material.CreateSurfaceOutput().ConnectToSource(
        shader.CreateOutput('surface', Sdf.ValueTypeNames.Token)
    )
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim())
    UsdShade.MaterialBindingAPI(mesh).Bind(material)
    shaders.append(shader)
  return stage, shaders


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument('--meshes', type=int, default=512)
  parser.add_argument('--edits', type=int, default=30)
  parser.add_argument('--resolution', type=int, default=32)
  parser.add_argument('--json', type=str, default='')
  args = parser.parse_args()

  native = os.environ.get('HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX', '0') != '1'
  mode = 'native' if native else 'emulated'

  stage, shaders = build_stage(args.meshes)
  test_helpers.create_render_settings(
      stage, resolution=(args.resolution, args.resolution), spp=1
  )

  engine = usd_render.RenderEngine(stage)
  engine.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin', camera_path='/root/camera'
  )
  t0 = time.perf_counter()
  engine.render()  # Populate + first commit.
  populate_s = time.perf_counter() - t0

  # Steady-state render cost with no pending edits (subtracted later).
  steady = []
  for _ in range(5):
    t0 = time.perf_counter()
    engine.render()
    steady.append(time.perf_counter() - t0)
  steady_render = statistics.median(steady)

  latencies = []
  for edit in range(args.edits):
    hue = (edit + 1) / (args.edits + 1)
    r, g, b = colorsys.hsv_to_rgb(hue, 0.8, 0.9)
    t0 = time.perf_counter()
    with Sdf.ChangeBlock():
      pass  # (edits below are individual; a change block would batch USD-side)
    for i, shader in enumerate(shaders):
      shade_hue = (hue + i / len(shaders)) % 1.0
      r, g, b = colorsys.hsv_to_rgb(shade_hue, 0.8, 0.9)
      shader.GetInput('diffuseColor').Set((r, g, b))
    edit_done = time.perf_counter()
    engine.render()
    end = time.perf_counter()
    # Paired baseline: an immediate re-render with nothing pending isolates
    # the update cost from render-time noise on loaded machines.
    engine.render()
    pair_end = time.perf_counter()
    latencies.append({
        'usd_edit': edit_done - t0,
        'update_and_render': end - edit_done,
        'paired_render': pair_end - end,
    })

  update_render = [l['update_and_render'] for l in latencies]
  usd_edit = [l['usd_edit'] for l in latencies]
  paired_update = [
      l['update_and_render'] - l['paired_render'] for l in latencies
  ]
  result = {
      'mode': mode,
      'meshes': args.meshes,
      'edits': args.edits,
      'populate_and_first_render_ms': populate_s * 1e3,
      'steady_render_ms': steady_render * 1e3,
      'usd_edit_ms_median': statistics.median(usd_edit) * 1e3,
      'update_and_render_ms_median': statistics.median(update_render) * 1e3,
      'update_and_render_ms_p10': sorted(update_render)[
          max(0, int(0.1 * len(update_render)) - 1)] * 1e3,
      'update_and_render_ms_p90': sorted(update_render)[
          int(0.9 * len(update_render))] * 1e3,
      'hydra_update_ms_median_est': (
          statistics.median(update_render) - steady_render) * 1e3,
      'hydra_update_ms_paired_median': statistics.median(paired_update) * 1e3,
  }
  print(json.dumps(result, indent=2))
  if args.json:
    with open(args.json, 'w') as f:
      json.dump(result, f, indent=2)
  return 0


if __name__ == '__main__':
  sys.exit(main())
