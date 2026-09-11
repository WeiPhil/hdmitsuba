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

"""Renders a stage's camera with a Hydra delegate via FrameRecorder.

Equivalent to `usdrecord --renderer <plugin> --camera <path>`, but driven
through UsdAppUtils.FrameRecorder directly so the tests do not depend on a
`usdrecord` script being installed: prebuilt USD distributions do not ship
one on every platform, and recent macOS ships an unrelated native
/usr/bin/usdrecord that shadows it on PATH. Run under a GL-capable display
(xvfb-run on headless Linux; native on macOS).

Usage: python storm_record_driver.py <scene> <camera> <width> <out-image>
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
  scene_path, camera_path, width, output_path = sys.argv[1:5]

  gl_state = _make_gl_context()  # noqa: F841 - keeps the context alive
  from pxr import Usd, UsdAppUtils, UsdGeom

  stage = Usd.Stage.Open(scene_path)
  camera = UsdGeom.Camera.Get(stage, camera_path)
  if not camera:
    print(f'FAIL: no camera at {camera_path}')
    return 1

  recorder = UsdAppUtils.FrameRecorder()
  recorder.SetRendererPlugin('HdStormRendererPlugin')
  recorder.SetImageWidth(int(width))
  if not recorder.Record(stage, camera, Usd.TimeCode.Default(), output_path):
    print('FAIL: FrameRecorder.Record returned false')
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
