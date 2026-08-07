// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include <gtest/gtest.h>
#include <mitsuba/core/fwd.h>
#include <mitsuba/core/spectrum.h>
#include <mitsuba/render/scene.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include "hdmitsuba/scene_index_backend.h"
#include "hdmitsuba/scene_manager.h"
#include "hdmitsuba/tests/test_util.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace hdmitsuba {
namespace {

using Float = float;
using Scene = mitsuba::Scene<Float, mitsuba::Color<Float, 3>>;
static MitsubaStaticState<Scene> global_static_state;

TEST(HdMitsubaSceneIndexTest, PluginRegistration) {
  UsdImagingStageSceneIndexRefPtr stage_scene_index =
      UsdImagingStageSceneIndex::New();
  HdSceneIndexBaseRefPtr terminal_scene_index =
      HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer(
          "HdMitsubaRendererPlugin", stage_scene_index);
  EXPECT_NE(terminal_scene_index, nullptr);
}

TEST(HdMitsubaSceneIndexTest, SceneIndexHarnessSync) {
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  SdfPath camera_path("/World/camera");
  UsdGeomCamera camera = UsdGeomCamera::Define(stage, camera_path);
  camera.GetFocalLengthAttr().Set(50.0f);

  SdfPath mesh_path("/World/mesh");
  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, mesh_path);
  VtVec3fArray points = {GfVec3f(0, 0, 0), GfVec3f(1, 0, 0), GfVec3f(0, 1, 0)};
  VtIntArray counts = {3};
  VtIntArray indices = {0, 1, 2};
  mesh.GetPointsAttr().Set(points);
  mesh.GetFaceVertexCountsAttr().Set(counts);
  mesh.GetFaceVertexIndicesAttr().Set(indices);

  SceneIndexTestHarness harness = CreateSceneIndexTestHarness(stage);
  harness.scene_manager->CommitResources();

  Scene* scene = dynamic_cast<Scene*>(harness.scene_manager->GetScene());
  ASSERT_NE(scene, nullptr);
  EXPECT_GE(scene->sensors().size(), 1);
  EXPECT_EQ(scene->sensors()[0]->id(), "/World/camera");
}

}  // namespace
}  // namespace hdmitsuba

PXR_NAMESPACE_CLOSE_SCOPE
