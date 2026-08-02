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

"""Tests for the hdMitsuba render delegate Materials."""

from __future__ import annotations

import mitsuba as mi
import numpy as np
import pytest

from pxr import Gf
from pxr import Sdf
from pxr import Usd
from pxr import UsdShade
import usd_render
from hdmitsuba.tests import test_helpers


@pytest.fixture(autouse=True)
def _set_mitsuba_variant():
  mi.set_variant('cuda_ad_rgb', 'llvm_ad_rgb')


@pytest.mark.parametrize(
    'scene_name',
    ['materials', 'checkerboard', 'normalmap', 'nodegraphs', 'display_color', 'mitsuba_bitmap']
)
def test_render(scene_name: str):
  stage = Usd.Stage.Open(
      f'{test_helpers.TEST_ASSETS_PATH}/materials/{scene_name}.usda'
  )
  camera_path = '/root/Camera/Camera'
  test_helpers.create_render_settings(stage)

  test_helpers.assert_hydra_equal_to_offline(
      stage, camera_path, f'test_render_{scene_name}', atol=0.05
  )


def test_texture_fallback():
  stage = Usd.Stage.Open(
      f'{test_helpers.TEST_ASSETS_PATH}/materials/texture_fallback.usda'
  )
  camera_path = '/root/Camera/Camera'
  test_helpers.create_render_settings(stage)

  image_hd, image_usd = test_helpers.assert_hydra_equal_to_offline(
      stage, camera_path, 'test_fallback_texture', spp=8, atol=0.05
  )

  info_msg = (
      'The fallback color being green central pixel should have its green'
      ' component dominant.'
  )
  center_pixel_hd = image_hd[70, 100, :3]
  assert center_pixel_hd[1] > center_pixel_hd[0], info_msg
  assert center_pixel_hd[1] > center_pixel_hd[2], info_msg

  center_pixel_usd = image_usd[70, 100, :3]
  assert center_pixel_usd[1] > center_pixel_usd[0], info_msg
  assert center_pixel_usd[1] > center_pixel_usd[2], info_msg


def test_texture_fallback_corrupt():
  stage = Usd.Stage.Open(
      f'{test_helpers.TEST_ASSETS_PATH}/materials/texture_fallback_corrupt.usda'
  )
  camera_path = '/root/Camera/Camera'
  test_helpers.create_render_settings(stage)

  image_hd, image_usd = test_helpers.assert_hydra_equal_to_offline(
      stage, camera_path, 'test_fallback_texture_corrupt', spp=8, atol=0.05
  )

  info_msg = (
      'The fallback color being blue central pixel should have its blue'
      ' component dominant.'
  )
  center_pixel_hd = image_hd[70, 100, :3]
  assert center_pixel_hd[2] > center_pixel_hd[0], info_msg
  assert center_pixel_hd[2] > center_pixel_hd[1], info_msg

  center_pixel_usd = image_usd[70, 100, :3]
  assert center_pixel_usd[2] > center_pixel_usd[0], info_msg
  assert center_pixel_usd[2] > center_pixel_usd[1], info_msg


def test_modify_material():
  stage = Usd.Stage.Open(
      f'{test_helpers.TEST_ASSETS_PATH}/materials/materials.usda'
  )
  session_layer = stage.GetSessionLayer()
  stage.SetEditTarget(session_layer)

  camera_path = '/root/Camera/Camera'
  test_helpers.create_render_settings(stage)

  engine = usd_render.RenderEngine(stage)
  engine.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin',
      camera_path=camera_path,
  )

  image_original = engine.render()['color']

  # Modify the Material by disconnecting the texture of the diffuse ground.
  # "ModifyMaterial" step
  shader = UsdShade.Shader.Get(
      stage, '/root/_materials/ground/Principled_BSDF'
  )
  assert shader is not None
  diffuse_color = shader.GetInput('diffuseColor')
  assert diffuse_color is not None
  diffuse_color.ClearSources()
  diffuse_color.Set(Gf.Vec3f(0.0, 0.0, 1.0))

  image_modified = engine.render()['color']
  test_helpers.write_image(
      image_original, 'test_modify_material_original.png'
  )
  test_helpers.write_image(
      image_modified, 'test_modify_material_modified.png'
  )

  assert np.mean(np.abs(image_modified - image_original)) > 0.02

  # "ResetMaterial" step
  # Reset the Material to its original state by removing the session override.
  diffuse_color.GetAttr().Clear()
  image_reset = engine.render()['color']
  test_helpers.robust_assert_close(image_reset, image_original, atol=0.05)


def test_modify_material_value_in_place():
  """Value-only edits update the Mitsuba BSDF in place.

  Editing only parameter values (colors, roughness) keeps the material's
  network structure unchanged, so the delegate applies the edit to the
  existing Mitsuba BSDF instead of rebuilding it. The rendered result must be
  indistinguishable from a from-scratch render of the edited stage.
  """
  stage = Usd.Stage.Open(f'{test_helpers.TEST_ASSETS_PATH}/shapes/cube.usda')
  # cube.usda binds /root/_materials/Material on the mesh prim itself; edit
  # that material's inputs so the edit is visible in the render.
  pbr = UsdShade.Shader.Get(
      stage, '/root/_materials/Material/Principled_BSDF'
  )
  assert pbr
  session_layer = stage.GetSessionLayer()
  stage.SetEditTarget(session_layer)
  color = pbr.GetInput('diffuseColor')
  assert color
  roughness = pbr.CreateInput('roughness', Sdf.ValueTypeNames.Float)
  roughness.Set(0.9)
  test_helpers.create_render_settings(stage, resolution=(160, 160))

  engine = usd_render.RenderEngine(stage)
  engine.configure(hydra_delegate_id='HdMitsubaRendererPlugin')
  image_original = engine.render()['color']

  color.Set(Gf.Vec3f(0.1, 0.2, 0.9))
  roughness.Set(0.2)
  image_edited = engine.render()['color']
  assert np.mean(np.abs(image_edited - image_original)) > 0.01

  fresh_engine = usd_render.RenderEngine(stage)
  fresh_engine.configure(hydra_delegate_id='HdMitsubaRendererPlugin')
  image_fresh = fresh_engine.render()['color']
  np.testing.assert_allclose(image_edited, image_fresh, atol=1e-4, rtol=1e-4)
def test_texture_color_space_via_network_schema():
  """The resolved color space reaches the translator via the network schema.

  In scene-index mode the UsdUVTexture sourceColorSpace input is folded by
  UsdImaging into the file parameter's colorSpace field of the Hydra 2.0
  material network schema (the parameter itself no longer appears in the
  network). An explicit "raw" must suppress the sRGB->linear decode on a
  color input, which brightens the texture relative to the sRGB
  interpretation.
  """
  stage = Usd.Stage.Open(
      f'{test_helpers.TEST_ASSETS_PATH}/materials/materials.usda'
  )
  test_helpers.create_render_settings(stage)
  camera_path = '/root/Camera/Camera'

  engine_srgb = usd_render.RenderEngine(stage)
  engine_srgb.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin', camera_path=camera_path
  )
  image_srgb = engine_srgb.render()['color']

  session_layer = stage.GetSessionLayer()
  stage.SetEditTarget(session_layer)
  texture = UsdShade.Shader.Get(
      stage, '/root/_materials/textured/Image_Texture'
  )
  assert texture
  texture.GetInput('sourceColorSpace').Set('raw')

  engine_raw = usd_render.RenderEngine(stage)
  engine_raw.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin', camera_path=camera_path
  )
  image_raw = engine_raw.render()['color']

  assert np.mean(np.abs(image_raw - image_srgb)) > 1e-4
  # Raw loading skips the sRGB->linear decode, which brightens the texture.
  assert np.mean(image_raw) > np.mean(image_srgb)
