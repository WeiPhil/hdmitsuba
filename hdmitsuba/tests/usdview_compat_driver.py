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

"""Drives hdMitsuba through the same UsdImagingGL stack usdview uses.

Run under a GL-capable display (xvfb-run on headless Linux; native on macOS). Exercises the application-side code
paths that the unit and fuzzing suites do not reach: the HdxTaskController
wiring, free-camera matrices (SetCameraState), application lighting state
(usdview's default camera light), AOV selection, and mid-session renderer
setting changes (including a Mitsuba variant switch, which recreates the
delegate's scene manager). Any Tf runtime/coding error in any stage is a
failure — this is exactly the class of problem that only shows up inside
usdview (e.g. the change tracker's "requires emulation" errors).

Usage: python usdview_compat_driver.py <scene.usda> <camera-prim-path|->
Exit code 0 iff every stage completed without Tf errors.
"""

import sys


def _make_gl_context():
  """Offscreen GL context + FBO, the same way usdrecord sets one up."""
  from PySide6.QtOpenGL import QOpenGLFramebufferObject
  from PySide6.QtOpenGL import QOpenGLFramebufferObjectFormat
  from PySide6.QtCore import QSize
  from PySide6.QtGui import QOffscreenSurface
  from PySide6.QtGui import QOpenGLContext
  from PySide6.QtGui import QSurfaceFormat
  from PySide6.QtWidgets import QApplication

  app = QApplication(sys.argv)
  fmt = QSurfaceFormat()
  fmt.setSamples(4)
  surface = QOffscreenSurface()
  surface.setFormat(fmt)
  surface.create()
  context = QOpenGLContext()
  context.setFormat(fmt)
  context.create()
  context.makeCurrent(surface)
  fbo = QOpenGLFramebufferObject(QSize(1, 1), QOpenGLFramebufferObjectFormat())
  fbo.bind()
  return app, surface, context, fbo


def main() -> int:
  scene_path = sys.argv[1]
  camera_path = sys.argv[2] if len(sys.argv) > 2 else '-'

  gl_state = _make_gl_context()  # noqa: F841 - keeps the context alive
  from pxr import Gf, Glf, Tf, Usd, UsdImagingGL

  stage = Usd.Stage.Open(scene_path)
  engine = UsdImagingGL.Engine()
  if not engine.SetRendererPlugin('HdMitsubaRendererPlugin'):
    print('FAIL: could not select the Mitsuba renderer plugin')
    return 1

  params = UsdImagingGL.RenderParams()
  params.frame = Usd.TimeCode.Default()
  engine.SetRenderViewport(Gf.Vec4d(0, 0, 96, 96))
  engine.SetRendererSetting('enableInteractive', False)

  failures = 0

  def stage_check(name, fn):
    nonlocal failures
    mark = Tf.Error.Mark()
    try:
      fn()
    except Tf.ErrorException as exc:
      print(f'FAIL [{name}]: Tf.ErrorException: {exc}')
      failures += 1
      mark.Clear()
      return
    errors = list(mark.GetErrors())
    if errors:
      print(f'FAIL [{name}]: {len(errors)} Tf errors')
      for error in errors[:5]:
        print('   ', error.commentary[:120])
      failures += 1
    else:
      print(f'ok   [{name}]')

  root = stage.GetPseudoRoot()

  # 1. Scene camera (if the scene has one) — the usdview "camera" view.
  if camera_path != '-':
    def render_scene_camera():
      params.camera = camera_path
      engine.Render(root, params)
      params.camera = ''
    stage_check('scene-camera render', render_scene_camera)

  # 2. Free camera: matrices fed via SetCameraState, exactly like usdview's
  #    auto-framing camera (becomes an engine-internal camera prim).
  def render_free_camera():
    view = Gf.Matrix4d().SetLookAt(
        Gf.Vec3d(3, 2, 3), Gf.Vec3d(0, 0, 0), Gf.Vec3d(0, 1, 0)
    )
    frustum = Gf.Frustum()
    frustum.SetPerspective(60.0, 1.0, 0.1, 100.0)
    engine.SetCameraState(view, frustum.ComputeProjectionMatrix())
    engine.Render(root, params)
  stage_check('free-camera render', render_free_camera)

  # 3. usdview's "Enable Default Camera Light": application lighting state.
  def render_camera_light():
    light = Glf.SimpleLight()
    light.position = Gf.Vec4f(3, 2, 3, 1)
    material = Glf.SimpleMaterial()
    engine.SetLightingState([light], material, Gf.Vec4f(0.1, 0.1, 0.1, 1.0))
    engine.Render(root, params)
  stage_check('default-camera-light render', render_camera_light)

  # 4. Every AOV the renderer advertises (usdview's AOV menu).
  for aov in engine.GetRendererAovs():
    def render_aov(aov=aov):
      if not engine.SetRendererAov(aov):
        raise RuntimeError(f'SetRendererAov({aov}) refused')
      engine.Render(root, params)
    stage_check(f'aov:{aov}', render_aov)

  # 5. Mid-session Mitsuba variant switch (usdview restoring a persisted
  #    renderer setting): the delegate recreates its scene manager and must
  #    rebuild the scene from cached specs without dirtying Hydra prims.
  def render_variant_switch():
    current = engine.GetRendererSetting('mitsuba:variant')
    target = 'scalar_rgb' if current != 'scalar_rgb' else 'llvm_rgb'
    engine.SetRendererSetting('mitsuba:variant', target)
    engine.Render(root, params)
    engine.Render(root, params)
  stage_check('variant-switch render', render_variant_switch)

  print('failures:', failures)
  return 1 if failures else 0


if __name__ == '__main__':
  sys.exit(main())
