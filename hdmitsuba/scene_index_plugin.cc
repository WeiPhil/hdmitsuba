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

#include "hdmitsuba/debug_codes.h"

#include <pxr/base/tf/envSetting.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName,
      "HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin"))
    ((clayPluginName, "HdMitsubaClaySceneIndexPlugin")));

TF_DEFINE_ENV_SETTING(HDMITSUBA_CLAY, false,
                      "Render every material as the default clay/gray BSDF "
                      "(a scene-level lookdev override implemented as a "
                      "filtering scene index).");

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


// -----------------------------------------------------------------------------
// Clay mode: a scene-level lookdev override.
//
// A minimal demonstration of what the Hydra 2.0 pipeline makes possible:
// this filter hides every material prim's data source, so downstream every
// material syncs as an empty network and the delegate's existing fallback
// turns the whole scene clay-gray — geometry, lights and bindings untouched.
//
// The Hydra 1.0 equivalent required editing the USD document or hacking each
// render delegate's material code. As a filter it is renderer-agnostic (a
// Storm-facing registration would clay Storm identically), composes with
// every other filter in the chain, and needs zero delegate changes.
// -----------------------------------------------------------------------------
TF_DECLARE_REF_PTRS(HdMitsubaClaySceneIndex);

class HdMitsubaClaySceneIndex final
    : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdMitsubaClaySceneIndexRefPtr New(
      const HdSceneIndexBaseRefPtr& input_scene) {
    return TfCreateRefPtr(new HdMitsubaClaySceneIndex(input_scene));
  }

  HdSceneIndexPrim GetPrim(const SdfPath& prim_path) const override {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(prim_path);
    if (prim.primType == HdPrimTypeTokens->material &&
        !IsEmissive(prim.dataSource)) {
      // No data source -> the material syncs as an empty network -> the
      // delegate substitutes its fallback (clay) BSDF. Emissive materials
      // pass through so mesh lights keep lighting the clay scene.
      prim.dataSource = nullptr;
    } else if ((prim.primType == HdPrimTypeTokens->mesh ||
                prim.primType == HdPrimTypeTokens->basisCurves) &&
               prim.dataSource) {
      // Scene color does not only come from materials: assets like
      // Kitchen_set are shaded entirely through displayColor primvars.
      // Overlay a constant clay-gray displayColor over the prim's data
      // source; every other field falls through to the original.
      prim.dataSource = HdOverlayContainerDataSource::New(
          GrayDisplayColorOverlay(), prim.dataSource);
    }
    return prim;
  }

  SdfPathVector GetChildPrimPaths(const SdfPath& prim_path) const override {
    return _GetInputSceneIndex()->GetChildPrimPaths(prim_path);
  }



  // primvars/displayColor -> a constant 18% gray, built once.
  static const HdContainerDataSourceHandle& GrayDisplayColorOverlay() {
    static const HdContainerDataSourceHandle overlay = [] {
      HdContainerDataSourceHandle primvar =
          HdPrimvarSchema::Builder()
              .SetPrimvarValue(
                  HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
                      VtVec3fArray{GfVec3f(0.18f)}))
              .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                  HdPrimvarSchemaTokens->constant))
              .SetRole(HdPrimvarSchema::BuildRoleDataSource(
                  HdPrimvarSchemaTokens->color))
              .Build();
      return HdRetainedContainerDataSource::New(
          HdPrimvarsSchema::GetSchemaToken(),
          HdRetainedContainerDataSource::New(
              HdTokens->displayColor, primvar));
    }();
    return overlay;
  }

  // A material whose network drives light emission (a nonzero emissiveColor
  // on any node) is a light source in clay terms and passes through.
  static bool IsEmissive(const HdContainerDataSourceHandle& data_source) {
    if (!data_source) {
      return false;
    }
    HdMaterialSchema material = HdMaterialSchema::GetFromParent(data_source);
    if (!material.IsDefined()) {
      return false;
    }
    static const TfToken kEmissiveColor("emissiveColor");
    for (const TfToken& context :
         {TfToken(), TfToken("mitsuba"), TfToken("__all")}) {
      HdMaterialNetworkSchema network = material.GetMaterialNetwork(context);
      if (!network.IsDefined()) {
        continue;
      }
      HdContainerDataSourceHandle nodes = network.GetNodes().GetContainer();
      if (!nodes) {
        continue;
      }
      for (const TfToken& node_name : nodes->GetNames()) {
        HdMaterialNodeSchema node(
            HdContainerDataSource::Cast(nodes->Get(node_name)));
        if (!node.IsDefined()) {
          continue;
        }
        HdContainerDataSourceHandle params = node.GetParameters().GetContainer();
        if (!params) {
          continue;
        }
        HdMaterialNodeParameterSchema param(HdContainerDataSource::Cast(
            params->Get(kEmissiveColor)));
        if (!param.IsDefined()) {
          continue;
        }
        if (HdSampledDataSourceHandle value = param.GetValue()) {
          VtValue v = value->GetValue(0.0f);
          if (v.IsHolding<GfVec3f>() &&
              v.UncheckedGet<GfVec3f>() != GfVec3f(0.0f)) {
            return true;
          }
        }
      }
    }
    return false;
  }

 protected:
  explicit HdMitsubaClaySceneIndex(const HdSceneIndexBaseRefPtr& input_scene)
      : HdSingleInputFilteringSceneIndexBase(input_scene) {}

  void _PrimsAdded(const HdSceneIndexBase& /*sender*/,
                   const HdSceneIndexObserver::AddedPrimEntries& entries)
      override {
    _SendPrimsAdded(entries);
  }
  void _PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                     const HdSceneIndexObserver::RemovedPrimEntries& entries)
      override {
    _SendPrimsRemoved(entries);
  }
  void _PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                     const HdSceneIndexObserver::DirtiedPrimEntries& entries)
      override {
    _SendPrimsDirtied(entries);
  }
};

class HdMitsubaClaySceneIndexPlugin final : public HdSceneIndexPlugin {
 public:
  HdMitsubaClaySceneIndexPlugin() = default;

 protected:
  HdSceneIndexBaseRefPtr _AppendSceneIndex(
      const HdSceneIndexBaseRefPtr& input_scene,
      const HdContainerDataSourceHandle& /*input_args*/) override {
    if (!TfGetEnvSetting(HDMITSUBA_CLAY)) {
      return input_scene;  // Disabled: zero overhead, filter not inserted.
    }
    TF_DEBUG(HDMITSUBA_NATIVE).Msg("Clay scene index inserted\n");
    return HdMitsubaClaySceneIndex::New(input_scene);
  }
};

TF_REGISTRY_FUNCTION(TfType) {
  HdSceneIndexPluginRegistry::Define<
      HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin>();
  HdSceneIndexPluginRegistry::Define<HdMitsubaClaySceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      /*rendererDisplayName=*/"Mitsuba", _tokens->sceneIndexPluginName,
      /*inputArgs=*/nullptr, /*insertionPhase=*/0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      /*rendererDisplayName=*/"Mitsuba", _tokens->clayPluginName,
      /*inputArgs=*/nullptr, /*insertionPhase=*/0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
