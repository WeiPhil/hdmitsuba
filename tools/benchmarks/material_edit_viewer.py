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

"""Interactive material-edit viewer: drag a slider, watch N materials update.

Every slider move edits the diffuseColor of every material in the scene
(one interactive tick touching all of them) and re-renders through the
hdMitsuba delegate, displaying the image and the edit->rendered latency.
The window title shows which backend consumed the edits:

  python material_edit_viewer.py --meshes 512
  HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX=1 python material_edit_viewer.py ...

The realtime feel comes from two layers of this branch: value-only edits
update the live Mitsuba BSDFs in place (no rebuilds), and on the native
backend the edits arrive as one deduplicated batch of scene index
notifications translated directly into specs.
"""

import argparse
import colorsys
import os
import sys
import time

import mitsuba as mi

mi.set_variant('cuda_ad_rgb', 'metal_ad_rgb', 'llvm_ad_rgb')

import numpy as np

from material_edit_benchmark import build_stage
from hdmitsuba.tests import test_helpers
import usd_render


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument('--meshes', type=int, default=512)
  parser.add_argument('--resolution', type=int, default=384)
  parser.add_argument('--spp', type=int, default=4)
  parser.add_argument('--self-test', action='store_true',
                      help='drive two slider ticks programmatically and exit')
  args = parser.parse_args()

  from PySide6.QtCore import Qt, QTimer
  from PySide6.QtGui import QImage, QPixmap
  from PySide6.QtWidgets import (QApplication, QLabel, QSlider, QVBoxLayout,
                                 QWidget)

  native = os.environ.get('HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX', '0') != '1'
  mode = 'native scene index backend' if native else 'emulated prim sync'

  stage, shaders = build_stage(args.meshes)
  test_helpers.create_render_settings(
      stage, resolution=(args.resolution, args.resolution), spp=args.spp
  )
  engine = usd_render.RenderEngine(stage)
  engine.configure(
      hydra_delegate_id='HdMitsubaRendererPlugin', camera_path='/root/camera'
  )
  engine.render()

  app = QApplication(sys.argv)
  window = QWidget()
  window.setWindowTitle(
      f'hdMitsuba — {args.meshes} materials — {mode}'
  )
  layout = QVBoxLayout(window)
  image_label = QLabel()
  image_label.setMinimumSize(args.resolution, args.resolution)
  hud = QLabel('drag the slider — every tick edits all materials')
  slider = QSlider(Qt.Horizontal)
  slider.setRange(0, 360)
  layout.addWidget(image_label)
  layout.addWidget(slider)
  layout.addWidget(hud)

  state = {'pending': None, 'busy': False}

  def show(image):
    rgb = np.clip(image[..., :3] ** (1 / 2.2) * 255, 0, 255).astype(np.uint8)
    rgb = np.ascontiguousarray(rgb)
    h, w, _ = rgb.shape
    qimg = QImage(rgb.data, w, h, 3 * w, QImage.Format_RGB888)
    image_label.setPixmap(QPixmap.fromImage(qimg))

  def apply_tick():
    if state['busy'] or state['pending'] is None:
      return
    state['busy'] = True
    hue = state['pending'] / 360.0
    state['pending'] = None
    t0 = time.perf_counter()
    for i, shader in enumerate(shaders):
      r, g, b = colorsys.hsv_to_rgb((hue + i / len(shaders)) % 1.0, 0.8, 0.9)
      shader.GetInput('diffuseColor').Set((r, g, b))
    image = engine.render()['color']
    latency = (time.perf_counter() - t0) * 1e3
    show(image)
    hud.setText(
        f'{args.meshes} materials edited -> rendered in {latency:.0f} ms'
        f'   [{mode}]'
    )
    state['busy'] = False
    if state['pending'] is not None:
      QTimer.singleShot(0, apply_tick)

  def on_slider(value):
    state['pending'] = value
    QTimer.singleShot(0, apply_tick)

  slider.valueChanged.connect(on_slider)
  show(engine.render()['color'])
  window.show()

  if args.self_test:
    QTimer.singleShot(300, lambda: slider.setValue(120))
    QTimer.singleShot(1800, lambda: slider.setValue(280))
    QTimer.singleShot(4000, app.quit)

  app.exec()
  return 0


if __name__ == '__main__':
  sys.exit(main())
