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

"""Cross-delegate comparisons against HdStorm.

The HdEmbree comparisons (camera_test.test_compare_to_hdembree and the
parametrized engine tests) only run where USD was built with the Embree
plugin, which prebuilt USD distributions usually omit. HdStorm ships with
every USD build, so these tests keep a cross-delegate check on camera
conventions — projection, transforms, apertures and aperture offsets, film
aspect — running everywhere. Shading is not comparable between a
rasterizer and a path tracer, so the comparison is the same one the Embree
test makes: the geometry silhouette (background/foreground mask) must
match to sub-half-percent pixel agreement.

Storm requires a GPU (an Hgi driver), which the in-process render engine
does not provide, so the Storm image is rendered in a subprocess through
UsdAppUtils.FrameRecorder (which creates its own Hgi) — the same machinery
usdrecord wraps — under xvfb on headless Linux, natively elsewhere.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

import mitsuba as mi
import numpy as np
import pytest

from pxr import Usd, UsdGeom
import usd_render
# Reuse the exact scene the HdEmbree comparison renders so the two suites
# validate the same conventions.
from hdmitsuba.tests import camera_test as camera_scenes
from hdmitsuba.tests import test_helpers

_WIDTH = 512
_DRIVER = os.path.join(os.path.dirname(__file__), 'storm_record_driver.py')


@pytest.fixture(autouse=True)
def _set_mitsuba_variant():
  mi.set_variant('cuda_ad_rgb', 'llvm_ad_rgb')


def _xvfb_prefix() -> list[str]:
  """A virtual X server is only needed on headless Linux; macOS renders
  natively."""
  return ['xvfb-run', '-a'] if sys.platform.startswith('linux') else []


def _have_storm_harness() -> bool:
  if sys.platform.startswith('linux') and shutil.which('xvfb-run') is None:
    return False
  try:
    import PySide6  # noqa: F401
  except ImportError:
    return False
  return True


def _render_storm(scene_path: str, output_path: str) -> np.ndarray:
  """Renders the scene camera with HdStorm and returns RGBA."""
  result = subprocess.run(
      _xvfb_prefix()
      + [sys.executable, _DRIVER, scene_path, '/camera', str(_WIDTH),
         output_path],
      env=os.environ.copy(),
      capture_output=True,
      text=True,
      timeout=300,
      check=False,
  )
  assert result.returncode == 0, (
      f'Storm record driver failed:\n{result.stdout}\n{result.stderr}'
  )
  return np.array(mi.Bitmap(output_path)).astype(np.float32)


def _make_stage() -> Usd.Stage:
  stage = Usd.Stage.CreateInMemory()
  camera_scenes._setup_stage(stage)
  # Author an exact 4:3 aperture so both stacks derive the identical image
  # height from the width (their rounding of fractional heights differs by
  # one row otherwise).
  camera = UsdGeom.Camera.Get(stage, '/camera')
  horizontal = camera.GetHorizontalApertureAttr().Get()
  camera.GetVerticalApertureAttr().Set(horizontal * 0.75)
  test_helpers.create_render_settings(stage)
  return stage


@pytest.mark.skipif(
    not _have_storm_harness(), reason='xvfb-run or usdrecord unavailable'
)
@pytest.mark.parametrize('modify_camera', [False, True],
                         ids=['default-camera', 'modified-camera'])
def test_camera_silhouette_matches_hdstorm(modify_camera: bool, tmp_path):
  if 'HdStormRendererPlugin' not in usd_render.get_registered_renderers():
    pytest.skip('HdStormRendererPlugin not available')

  stage = _make_stage()
  if modify_camera:
    # Also validate focal-length and transform conventions, not just the
    # default framing.
    camera_scenes._modify_camera(
        stage, '/camera', focal_length_scale=1.4, translation=(0.4, 0.2, 0.3)
    )

  engine = usd_render.RenderEngine(stage)
  engine.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin',
      width=_WIDTH,
      camera_path='/camera',
  )
  mitsuba_image = engine.render()['color']
  test_helpers.write_image(
      mitsuba_image, f'storm_compat_{modify_camera}_mitsuba.png'
  )

  scene_path = str(tmp_path / 'scene.usda')
  stage.Export(scene_path)
  storm_image = _render_storm(scene_path, str(tmp_path / 'storm.exr'))

  assert storm_image.shape == mitsuba_image.shape, (
      f'image shapes diverged: storm {storm_image.shape} vs mitsuba'
      f' {mitsuba_image.shape}'
  )

  # Background is bright (dome light) in the Mitsuba render and has zero
  # alpha in the Storm render; the black plane is the foreground in both.
  mitsuba_background = mitsuba_image[..., 0] > 0.5
  storm_background = storm_image[..., 3] < 0.5
  fraction_mismatch = np.mean(storm_background != mitsuba_background)
  assert fraction_mismatch < 0.005, (
      f'silhouette mismatch on {fraction_mismatch:.2%} of pixels'
  )
