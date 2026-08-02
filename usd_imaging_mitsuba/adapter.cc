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

// Hydra 2.0 support: a "keyless" UsdImagingAPISchemaAdapter that publishes
// custom `mitsuba:*` USD attributes into the stage scene index.
//
// The legacy UsdImagingDelegate satisfied HdSceneDelegate::Get /
// GetCameraParamValue for arbitrary attribute names by reading the USD prim
// directly. The Hydra 2.0 UsdImagingStageSceneIndex is schema-driven and only
// publishes data provided by adapters, so without this adapter the delegate's
// custom attributes (`mitsuba:subdivision_level`, `mitsuba:sensor`,
// `mitsuba:sensor:type`, ...) silently vanish when an application (e.g.
// usdview in USD >= 24.x) uses the scene-index front-end.
//
// Keyless adapters are constructed through
// UsdImagingAdapterRegistry::ConstructKeylessAPISchemaAdapters() and are
// invoked for every prim; they are registered like regular API schema
// adapters but with an empty `apiSchemaName` in plugInfo.json.
//
// Data layout produced by this adapter:
//  - Camera prims: the attributes are overlaid into the `camera` container
//    (HdCameraSchema), keyed by their full name. This makes the emulated
//    HdSceneDelegate::GetCameraParamValue(id, "mitsuba:sensor:type") work
//    unchanged, and also makes the values available to data-source consumers.
//  - All other prims: the attributes are published as top-level entries of the
//    prim's container data source, keyed by their full name (e.g.
//    "mitsuba:subdivision_level"). The hdMitsuba prims read them from the
//    render index's terminal scene index.

#include <string>
#include <vector>

#include <pxr/base/tf/registryManager.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/type.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/pxr.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdLux/boundableLightBase.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/nonboundableLightBase.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#include <pxr/usdImaging/usdImaging/dataSourceAttribute.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr char kMitsubaAttrPrefix[] = "mitsuba:";

bool IsMitsubaAttributeName(const TfToken& name) {
  return TfStringStartsWith(name.GetString(), kMitsubaAttrPrefix);
}

std::vector<UsdAttribute> GetAuthoredMitsubaAttributes(const UsdPrim& prim) {
  std::vector<UsdAttribute> result;
  for (const UsdAttribute& attr : prim.GetAuthoredAttributes()) {
    if (IsMitsubaAttributeName(attr.GetName()) && attr.HasValue()) {
      result.push_back(attr);
    }
  }
  return result;
}

bool IsLightPrim(const UsdPrim& prim) {
  return prim.IsA<UsdLuxBoundableLightBase>() ||
         prim.IsA<UsdLuxNonboundableLightBase>() ||
         prim.HasAPI<UsdLuxLightAPI>();
}

// Light parameters that are not `inputs:` namespaced (structural attributes
// such as UsdLuxSphereLight's treatAsPoint) are not reliably invalidated by
// the stock UsdImaging light adapters when they are newly authored; we cover
// them here so that interactive edits reach the delegate.
bool IsLightParameterName(const TfToken& name) {
  return TfStringStartsWith(name.GetString(), "inputs:") ||
         name == TfToken("treatAsPoint");
}

}  // namespace

class UsdImagingMitsubaAttributesAdapter final
    : public UsdImagingAPISchemaAdapter {
 public:
  HdContainerDataSourceHandle GetImagingSubprimData(
      const UsdPrim& prim, const TfToken& subprim,
      const TfToken& appliedInstanceName,
      const UsdImagingDataSourceStageGlobals& stageGlobals) override {
    // Only contribute to the primary ("") subprim of concrete prims.
    if (!subprim.IsEmpty() || !appliedInstanceName.IsEmpty()) {
      return nullptr;
    }
    const std::vector<UsdAttribute> attrs = GetAuthoredMitsubaAttributes(prim);
    if (attrs.empty()) {
      return nullptr;
    }

    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> sources;
    names.reserve(attrs.size());
    sources.reserve(attrs.size());
    for (const UsdAttribute& attr : attrs) {
      HdSampledDataSourceHandle value_source =
          UsdImagingDataSourceAttributeNew(attr, stageGlobals,
                                           prim.GetPath(),
                                           HdDataSourceLocator(attr.GetName()));
      if (!value_source) {
        continue;
      }
      names.push_back(attr.GetName());
      sources.push_back(value_source);
    }
    if (names.empty()) {
      return nullptr;
    }

    HdContainerDataSourceHandle values = HdRetainedContainerDataSource::New(
        names.size(), names.data(), sources.data());

    if (prim.IsA<UsdGeomCamera>()) {
      // Overlay into the camera schema container so that the emulated
      // HdSceneDelegate::GetCameraParamValue() resolves the custom params.
      return HdRetainedContainerDataSource::New(
          HdCameraSchema::GetSchemaToken(), values);
    }
    return values;
  }

  HdDataSourceLocatorSet InvalidateImagingSubprim(
      const UsdPrim& prim, const TfToken& subprim,
      const TfToken& appliedInstanceName, const TfTokenVector& properties,
      UsdImagingPropertyInvalidationType /*invalidationType*/) override {
    if (!subprim.IsEmpty() || !appliedInstanceName.IsEmpty()) {
      return HdDataSourceLocatorSet();
    }
    HdDataSourceLocatorSet result;
    const bool is_light = IsLightPrim(prim);
    const bool is_material = prim.IsA<UsdShadeMaterial>();
    for (const TfToken& property : properties) {
      if (is_material &&
          TfStringStartsWith(property.GetString(), "outputs:")) {
        // Material terminal (re)connections and disconnections are not
        // reliably invalidated by the stock adapters (e.g. clearing the
        // surface output's connection); map them to the material schema so
        // that the material sprim re-syncs.
        result.insert(HdMaterialSchema::GetDefaultLocator());
        continue;
      }
      if (is_light && IsLightParameterName(property)) {
        // Covers light parameters missed by the stock adapters (e.g. a newly
        // authored treatAsPoint); maps to DirtyParams on the light sprim.
        result.insert(HdLightSchema::GetDefaultLocator());
        // Mesh lights (LightAPI applied to a Mesh prim) consume their light
        // parameters during the mesh rprim's sync, so dirty the mesh
        // topology as well to re-run it.
        if (prim.IsA<UsdGeomMesh>()) {
          result.insert(HdMeshSchema::GetTopologyLocator());
        }
        continue;
      }
      if (!IsMitsubaAttributeName(property)) {
        continue;
      }
      if (prim.IsA<UsdGeomCamera>()) {
        // Maps to HdCamera::DirtyParams through the emulation layer.
        result.insert(HdCameraSchema::GetDefaultLocator());
      } else {
        // The custom attribute itself (for data-source-native consumers),
        // plus the mesh topology locator, which the emulation layer
        // translates to HdChangeTracker::DirtyTopology — the bit that makes
        // HdMitsubaMesh re-run SyncTopology/UpdateScene, where the custom
        // attributes (e.g. mitsuba:subdivision_level, mitsuba:sensor) are
        // consumed.
        result.insert(HdDataSourceLocator(property));
        result.insert(HdMeshSchema::GetTopologyLocator());
      }
    }
    return result;
  }
};

TF_REGISTRY_FUNCTION(TfType) {
  TfType t = TfType::Define<UsdImagingMitsubaAttributesAdapter,
                            TfType::Bases<UsdImagingAPISchemaAdapter>>();
  t.SetFactory<
      UsdImagingAPISchemaAdapterFactory<UsdImagingMitsubaAttributesAdapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
