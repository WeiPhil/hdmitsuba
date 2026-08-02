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

"""Application-level (usdview wiring) compliance tests for hdMitsuba.

The unit and fuzzing suites drive the delegate through the render_engine;
usdview drives it through UsdImagingGL + HdxTaskController, which exercises
different code paths (free cameras, application lighting, AOV selection,
renderer settings pushed mid-session). These tests run that stack headless
and fail on any Tf error — the class of breakage that historically only
surfaced when someone launched usdview.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

import mitsuba as mi
import numpy as np
import pytest

from pxr import Sdf
from pxr import Usd
import usd_render
from hdmitsuba.tests import test_helpers

_DRIVER = os.path.join(os.path.dirname(__file__), 'usdview_compat_driver.py')


@pytest.fixture(autouse=True)
def _set_mitsuba_variant():
  mi.set_variant('cuda_ad_rgb', 'llvm_ad_rgb')


def _xvfb_prefix() -> list[str]:
  """A virtual X server is only needed on headless Linux; macOS renders
  natively."""
  return ['xvfb-run', '-a'] if sys.platform.startswith('linux') else []


def _have_gl_harness() -> bool:
  if sys.platform.startswith('linux') and shutil.which('xvfb-run') is None:
    return False
  try:
    import PySide6  # noqa: F401
  except ImportError:
    return False
  return True


def _run_driver(extra_env: dict[str, str]) -> subprocess.CompletedProcess:
  scene = f'{test_helpers.TEST_ASSETS_PATH}/materials/materials.usda'
  env = os.environ.copy()
  env.update(extra_env)
  return subprocess.run(
      _xvfb_prefix()
      + [
          sys.executable,
          _DRIVER,
          scene,
          '/root/Camera/Camera',
      ],
      env=env,
      capture_output=True,
      text=True,
      timeout=600,
      check=False,
  )


@pytest.mark.skipif(
    not _have_gl_harness(), reason='xvfb-run or PySide6 unavailable'
)
@pytest.mark.parametrize(
    'wiring_env',
    [
        {},
        {'USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX_OBSERVER_RENDERER': '1'},
    ],
    ids=['default-wiring', 'observer-wiring'],
)
def test_usdview_stack_has_no_tf_errors(wiring_env: dict[str, str]):
  """The full usdview stack runs every stage without Tf errors."""
  result = _run_driver(wiring_env)
  output = result.stdout + result.stderr
  assert result.returncode == 0, (
      f'usdview-compat driver failed (exit {result.returncode}):\n{output}'
  )
  assert 'requires emulation' not in output, output


def test_variant_switch_preserves_scene():
  """A mid-session Mitsuba variant switch keeps rendering the same scene.

  usdview restores persisted renderer settings after the delegate is
  created, so `mitsuba:variant` can change mid-session. The delegate then
  recreates its scene manager and must rebuild the Mitsuba scene from its
  cached specs — without re-syncing prims through Hydra (the change
  tracker's legacy dirtying API is unavailable to scene-index-fed prims).
  """
  stage = Usd.Stage.Open(f'{test_helpers.TEST_ASSETS_PATH}/shapes/cube.usda')
  test_helpers.create_render_settings(stage, resolution=(128, 128))
  settings_prim = stage.GetPrimAtPath('/Render/PrimarySettings')
  variant_attr = settings_prim.CreateAttribute(
      'mitsuba:variant', Sdf.ValueTypeNames.String
  )

  engine = usd_render.RenderEngine(stage)
  engine.configure(hydra_delegate_id='HdMitsubaRendererPlugin')
  image_before = engine.render()['color']
  assert np.mean(image_before[..., :3]) > 0.01, 'initial render is empty'

  variant_attr.Set('scalar_rgb')
  image_after = engine.render()['color']

  # Same scene, same camera, different Mitsuba backend: images must agree
  # up to backend numerics/sampling.
  test_helpers.robust_assert_close(
      image_after[..., :3], image_before[..., :3], atol=0.05
  )
