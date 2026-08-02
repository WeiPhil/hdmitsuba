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

"""Clay mode: the scene-level material override implemented as a filter.

HDMITSUBA_CLAY=1 inserts HdMitsubaClaySceneIndex into the scene index
chain, which hides every non-emissive material's data source; the delegate
then substitutes its fallback BSDF everywhere while geometry, bindings and
lights (including emissive-material mesh lights) stay intact. TfEnvSetting
values are cached per process, so each mode renders in a subprocess.
"""

import json
import os
import subprocess
import sys

import numpy as np
import pytest

_DRIVER = r'''
import json, sys
import mitsuba as mi
mi.set_variant('cuda_ad_rgb', 'llvm_ad_rgb')
import numpy as np
from pxr import Sdf, Usd, UsdGeom, UsdShade
import usd_render
from hdmitsuba.tests import test_helpers

if sys.argv[1] == 'DISPLAYCOLOR':
  # A scene shaded purely through displayColor primvars (no materials),
  # like Pixar's Kitchen_set.
  stage = Usd.Stage.CreateInMemory()
  UsdGeom.Xform.Define(stage, '/root')
  from pxr import UsdLux
  UsdLux.DomeLight.Define(stage, '/root/dome')
  camera = UsdGeom.Camera.Define(stage, '/root/cam')
  UsdGeom.XformCommonAPI(camera.GetPrim()).SetTranslate((0.0, 0.0, 8.0))
  for i, color in enumerate([(1, 0.05, 0.05), (0.05, 1, 0.05)]):
    mesh = UsdGeom.Mesh.Define(stage, f'/root/m{i}')
    x = i * 2.5 - 1.25
    mesh.CreatePointsAttr([(x - 1, -1, 0), (x + 1, -1, 0), (x + 1, 1, 0),
                           (x - 1, 1, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateDisplayColorAttr([color])
else:
  stage = Usd.Stage.Open(sys.argv[1])
test_helpers.create_render_settings(stage, resolution=(96, 96))
camera = None
for prim in stage.Traverse():
  if prim.IsA(UsdGeom.Camera):
    camera = str(prim.GetPath())
    break
engine = usd_render.RenderEngine(stage)
kwargs = {'hydra_delegate_id': 'HdMitsubaRendererPlugin'}
if camera:
  kwargs['camera_path'] = camera
engine.configure(**kwargs)
image = engine.render()['color'][..., :3]
print(json.dumps({
    'mean': float(np.mean(image)),
    'rg_saturation': float(np.mean(np.abs(image[..., 0] - image[..., 1]))),
}))
'''


def _render(scene: str, clay: bool) -> dict:
  env = os.environ.copy()
  if clay:
    env['HDMITSUBA_CLAY'] = 'true'
  else:
    env.pop('HDMITSUBA_CLAY', None)
  result = subprocess.run(
      [sys.executable, '-c', _DRIVER, scene],
      env=env,
      capture_output=True,
      text=True,
      timeout=600,
      check=False,
  )
  assert result.returncode == 0, result.stdout + result.stderr
  return json.loads(result.stdout.strip().splitlines()[-1])


def test_clay_mode_neutralizes_display_color():
  # Color authored as displayColor primvars (no materials at all) must also
  # turn clay -- assets like Kitchen_set are shaded entirely this way.
  normal = _render('DISPLAYCOLOR', clay=False)
  clay = _render('DISPLAYCOLOR', clay=True)
  assert clay['mean'] > 0.005, clay
  assert clay['rg_saturation'] < normal['rg_saturation'] * 0.3, (normal, clay)


def test_clay_mode_desaturates_but_keeps_mesh_lights():
  scene = os.path.join(
      os.path.dirname(__file__), '..', 'test_assets', 'materials',
      'nodegraphs.usda'
  )
  if not os.path.exists(scene):
    pytest.skip('scene asset unavailable')
  normal = _render(scene, clay=False)
  clay = _render(scene, clay=True)

  # The scene still renders (lights survive the override)...
  assert clay['mean'] > 0.005, clay
  # ...but colored materials are gone: red/green channel separation drops
  # to a fraction of the lit original's.
  assert clay['rg_saturation'] < normal['rg_saturation'] * 0.6, (
      normal,
      clay,
  )
