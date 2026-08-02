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

#include <pxr/base/tf/registryManager.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName,
      "HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin")));

// Scene index plugin appended for the Mitsuba renderer by the render index
// itself (HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer), so any
// scene-index host — usdview, usdrecord, the render_engine — gets it without
// host-side wiring.
//
// The filter evaluates ext computations (e.g. UsdSkel skinning) and presents
// their outputs as plain authored primvars, so the mesh rprim's regular
// primvar reads see skinned points and the delegate needs no ext-computation
// machinery of its own on scene-index hosts.
class HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin final
    : public HdSceneIndexPlugin {
 public:
  HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin() = default;

 protected:
  HdSceneIndexBaseRefPtr _AppendSceneIndex(
      const HdSceneIndexBaseRefPtr& input_scene,
      const HdContainerDataSourceHandle& /*input_args*/) override {
    return HdSiExtComputationPrimvarPruningSceneIndex::New(input_scene);
  }
};

TF_REGISTRY_FUNCTION(TfType) {
  HdSceneIndexPluginRegistry::Define<
      HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      /*rendererDisplayName=*/"Mitsuba", _tokens->sceneIndexPluginName,
      /*inputArgs=*/nullptr, /*insertionPhase=*/0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
