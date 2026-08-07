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

#include "hdmitsuba/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/trace/trace.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/geomSubset.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/subdivisionTagsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/pxr.h>

#include "hdmitsuba/debug_codes.h"
#include "hdmitsuba/instancer.h"
#include "hdmitsuba/mesh/geometry_processor.h"
#include "hdmitsuba/mesh/subdivision.h"
#include "hdmitsuba/prim_translation.h"
#include "hdmitsuba/render_delegate.h"
#include "hdmitsuba/render_param.h"
#include "hdmitsuba/scene_manager.h"
#include "hdmitsuba/spec_types.h"
#include "hdmitsuba/utils.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

bool ValidatePrimvarSize(const VtValue& value, HdInterpolation interpolation,
                         size_t vertex_count, size_t face_count,
                         size_t corner_count) {
  if (value.IsEmpty()) return false;

  size_t size = 0;
  if (value.IsHolding<VtVec3fArray>()) {
    size = value.Get<VtVec3fArray>().size();
  } else if (value.IsHolding<VtVec2fArray>()) {
    size = value.Get<VtVec2fArray>().size();
  } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec2f>()) {
    size = 1;
  } else {
    return true;
  }

  switch (interpolation) {
    case HdInterpolationConstant:
      return size == 1;
    case HdInterpolationUniform:
      return size == face_count;
    case HdInterpolationVertex:
      return size == vertex_count;
    case HdInterpolationFaceVarying:
      return size == corner_count;
    default:
      return false;
  }
}

// Returns the render index's terminal scene index (the Hydra 2.0 view of the
// scene), or null when unavailable.
HdSceneIndexBaseRefPtr GetTerminalSceneIndex(HdSceneDelegate* sceneDelegate) {
  return sceneDelegate->GetRenderIndex().GetTerminalSceneIndex();
}

// Returns the prim's container data source on the terminal scene index, or
// null when unavailable.
HdContainerDataSourceHandle GetTerminalPrimDataSource(
    HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  if (HdSceneIndexBaseRefPtr scene_index =
          GetTerminalSceneIndex(sceneDelegate)) {
    return scene_index->GetPrim(id).dataSource;
  }
  return nullptr;
}

// Reads a custom (non-schema) attribute value for a prim.
//
// Hydra 2.0: when the scene is fed by the UsdImagingStageSceneIndex, custom
// `mitsuba:*` attributes are published as top-level entries of the prim's
// container data source by the keyless UsdImagingMitsubaAttributesAdapter
// (see usd_imaging_mitsuba/adapter.cc), so we look them up on the render
// index's terminal scene index first. When running behind a legacy scene
// delegate, the data source lookup misses and we fall back to
// HdSceneDelegate::Get(), which reads the USD attribute directly.
VtValue GetCustomPrimValue(HdSceneDelegate* sceneDelegate, const SdfPath& id,
                           const TfToken& key) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    if (HdSampledDataSourceHandle sampled =
            HdSampledDataSource::Cast(data_source->Get(key))) {
      VtValue value = sampled->GetValue(0.0f);
      if (!value.IsEmpty()) {
        return value;
      }
    }
  }
  return VtValue();
}

// The following readers are data-source-first (Hydra 2.0 native), with the
// legacy scene-delegate call kept as a fallback for hosts that still drive
// the delegate through a legacy front-end.

bool GetMeshVisibility(HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdVisibilitySchema schema = HdVisibilitySchema::GetFromParent(data_source);
    if (schema.IsDefined()) {
      if (HdBoolDataSourceHandle visibility = schema.GetVisibility()) {
        return visibility->GetTypedValue(0.0f);
      }
    }
  }
  // An absent visibility schema means visible.
  return true;
}

GfMatrix4d GetMeshTransform(HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdXformSchema schema = HdXformSchema::GetFromParent(data_source);
    if (schema.IsDefined()) {
      if (HdMatrixDataSourceHandle matrix = schema.GetMatrix()) {
        return matrix->GetTypedValue(0.0f);
      }
    }
  }
  // No xform published: identity.
  return GfMatrix4d(1.0);
}

// Resolves the bound material path from a prim data source's material
// bindings schema (used for both meshes and geom subset child prims).
std::optional<SdfPath> GetMaterialIdFromDataSource(
    const HdContainerDataSourceHandle& data_source) {
  HdMaterialBindingsSchema bindings =
      HdMaterialBindingsSchema::GetFromParent(data_source);
  if (bindings.IsDefined()) {
    // The all-purpose binding; application-side filters (e.g. usdview's
    // material binding resolving scene index) resolve purpose-specific
    // bindings into it. Fall back to our declared binding purpose.
    HdMaterialBindingSchema binding = bindings.GetMaterialBinding();
    if (!binding.IsDefined()) {
      binding = bindings.GetMaterialBinding(HdTokens->full);
    }
    if (binding.IsDefined()) {
      if (HdPathDataSourceHandle path = binding.GetPath()) {
        return path->GetTypedValue(0.0f);
      }
    }
  }
  return std::nullopt;
}

SdfPath GetMeshMaterialId(HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    if (std::optional<SdfPath> material_id =
            GetMaterialIdFromDataSource(data_source)) {
      return *material_id;
    }
  }
  return SdfPath();  // No binding published: the mesh is unbound.
}

// Schema-native mesh topology: assembled from HdMeshSchema (face vertex
// counts/indices, holes, orientation, subdivision scheme) plus geom subsets,
// which Hydra 2.0 represents as `geomSubset` child prims of the mesh. Returns
// nullopt when the terminal scene index has no mesh topology for the prim so
// the caller can fall back to the scene delegate.
std::optional<HdMeshTopology> GetMeshTopologyFromSceneIndex(
    HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  HdSceneIndexBaseRefPtr scene_index = GetTerminalSceneIndex(sceneDelegate);
  if (!scene_index) {
    return std::nullopt;
  }
  HdContainerDataSourceHandle data_source =
      scene_index->GetPrim(id).dataSource;
  if (!data_source) {
    return std::nullopt;
  }
  HdMeshSchema mesh = HdMeshSchema::GetFromParent(data_source);
  HdMeshTopologySchema topology_schema = mesh.GetTopology();
  if (!topology_schema.IsDefined()) {
    return std::nullopt;
  }

  VtIntArray face_vertex_counts;
  if (HdIntArrayDataSourceHandle counts =
          topology_schema.GetFaceVertexCounts()) {
    face_vertex_counts = counts->GetTypedValue(0.0f);
  }
  VtIntArray face_vertex_indices;
  if (HdIntArrayDataSourceHandle indices =
          topology_schema.GetFaceVertexIndices()) {
    face_vertex_indices = indices->GetTypedValue(0.0f);
  }
  VtIntArray hole_indices;
  if (HdIntArrayDataSourceHandle holes = topology_schema.GetHoleIndices()) {
    hole_indices = holes->GetTypedValue(0.0f);
  }
  TfToken orientation = HdMeshTopologySchemaTokens->rightHanded;
  if (HdTokenDataSourceHandle orientation_source =
          topology_schema.GetOrientation()) {
    orientation = orientation_source->GetTypedValue(0.0f);
  }
  TfToken scheme = PxOsdOpenSubdivTokens->none;
  if (HdTokenDataSourceHandle scheme_source = mesh.GetSubdivisionScheme()) {
    scheme = scheme_source->GetTypedValue(0.0f);
  }

  HdMeshTopology topology(scheme, orientation, face_vertex_counts,
                          face_vertex_indices, hole_indices);

  // Geom subsets are children of the mesh prim in the scene index.
  HdGeomSubsets geom_subsets;
  for (const SdfPath& child_path : scene_index->GetChildPrimPaths(id)) {
    HdSceneIndexPrim child = scene_index->GetPrim(child_path);
    if (child.primType != HdPrimTypeTokens->geomSubset || !child.dataSource) {
      continue;
    }
    HdGeomSubsetSchema subset_schema =
        HdGeomSubsetSchema::GetFromParent(child.dataSource);
    if (!subset_schema.IsDefined()) {
      continue;
    }
    HdTokenDataSourceHandle type = subset_schema.GetType();
    if (!type ||
        type->GetTypedValue(0.0f) != HdGeomSubsetSchemaTokens->typeFaceSet) {
      continue;
    }
    HdGeomSubset subset;
    subset.type = HdGeomSubset::TypeFaceSet;
    subset.id = child_path;
    if (HdIntArrayDataSourceHandle indices = subset_schema.GetIndices()) {
      subset.indices = indices->GetTypedValue(0.0f);
    }
    subset.materialId =
        GetMaterialIdFromDataSource(child.dataSource).value_or(SdfPath());
    geom_subsets.push_back(std::move(subset));
  }
  if (!geom_subsets.empty()) {
    topology.SetGeomSubsets(geom_subsets);
  }
  return topology;
}

// Subdivision tags from HdMeshSchema's subdivisionTags container; an absent
// container on a real data source means "no tags" (matching USD authoring).
PxOsdSubdivTags GetMeshSubdivTags(HdSceneDelegate* sceneDelegate,
                                  const SdfPath& id) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    PxOsdSubdivTags tags;
    HdSubdivisionTagsSchema schema =
        HdMeshSchema::GetFromParent(data_source).GetSubdivisionTags();
    if (schema.IsDefined()) {
      if (HdTokenDataSourceHandle t = schema.GetInterpolateBoundary()) {
        tags.SetVertexInterpolationRule(t->GetTypedValue(0.0f));
      }
      if (HdTokenDataSourceHandle t =
              schema.GetFaceVaryingLinearInterpolation()) {
        tags.SetFaceVaryingInterpolationRule(t->GetTypedValue(0.0f));
      }
      if (HdTokenDataSourceHandle t = schema.GetTriangleSubdivisionRule()) {
        tags.SetTriangleSubdivision(t->GetTypedValue(0.0f));
      }
      if (HdIntArrayDataSourceHandle v = schema.GetCornerIndices()) {
        tags.SetCornerIndices(v->GetTypedValue(0.0f));
      }
      if (HdFloatArrayDataSourceHandle v = schema.GetCornerSharpnesses()) {
        tags.SetCornerWeights(v->GetTypedValue(0.0f));
      }
      if (HdIntArrayDataSourceHandle v = schema.GetCreaseIndices()) {
        tags.SetCreaseIndices(v->GetTypedValue(0.0f));
      }
      if (HdIntArrayDataSourceHandle v = schema.GetCreaseLengths()) {
        tags.SetCreaseLengths(v->GetTypedValue(0.0f));
      }
      if (HdFloatArrayDataSourceHandle v = schema.GetCreaseSharpnesses()) {
        tags.SetCreaseWeights(v->GetTypedValue(0.0f));
      }
    }
    return tags;
  }
  return PxOsdSubdivTags();
}

// The refine level from the (legacy) display style schema, which application
// filters (e.g. usdview's complexity setting and the engine's
// HdsiLegacyDisplayStyleOverrideSceneIndex) write into the scene index.
int GetMeshRefineLevel(HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdLegacyDisplayStyleSchema schema =
        HdLegacyDisplayStyleSchema::GetFromParent(data_source);
    if (schema.IsDefined()) {
      if (HdIntDataSourceHandle refine_level = schema.GetRefineLevel()) {
        return refine_level->GetTypedValue(0.0f);
      }
    }
  }
  return 0;
}

std::optional<HdInterpolation> InterpolationFromToken(const TfToken& token) {
  if (token == HdPrimvarSchemaTokens->constant) return HdInterpolationConstant;
  if (token == HdPrimvarSchemaTokens->uniform) return HdInterpolationUniform;
  if (token == HdPrimvarSchemaTokens->varying) return HdInterpolationVarying;
  if (token == HdPrimvarSchemaTokens->vertex) return HdInterpolationVertex;
  if (token == HdPrimvarSchemaTokens->faceVarying) {
    return HdInterpolationFaceVarying;
  }
  if (token == HdPrimvarSchemaTokens->instance) return HdInterpolationInstance;
  return std::nullopt;
}

using PrimvarDescriptorMap =
    absl::flat_hash_map<TfToken, HdPrimvarDescriptor, TfToken::HashFunctor>;

// All primvar descriptors of the prim, keyed by name. Schema-native: a single
// pass over HdPrimvarsSchema (no per-interpolation queries); falls back to
// per-interpolation scene delegate queries for delegate-fed hosts.
PrimvarDescriptorMap GetAllMeshPrimvarDescriptors(
    HdSceneDelegate* sceneDelegate, const SdfPath& id) {
  PrimvarDescriptorMap descriptors;
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(data_source);
    if (primvars.IsDefined()) {
      for (const TfToken& name : primvars.GetPrimvarNames()) {
        HdPrimvarSchema primvar = primvars.GetPrimvar(name);
        if (!primvar.IsDefined()) {
          continue;
        }
        HdTokenDataSourceHandle interpolation_source =
            primvar.GetInterpolation();
        if (!interpolation_source) {
          continue;
        }
        std::optional<HdInterpolation> interpolation =
            InterpolationFromToken(interpolation_source->GetTypedValue(0.0f));
        if (!interpolation.has_value()) {
          continue;
        }
        HdPrimvarDescriptor descriptor;
        descriptor.name = name;
        descriptor.interpolation = *interpolation;
        if (HdTokenDataSourceHandle role = primvar.GetRole()) {
          descriptor.role = role->GetTypedValue(0.0f);
        }
        descriptor.indexed =
            primvar.GetIndexedPrimvarValue() && primvar.GetIndices();
        descriptors[name] = descriptor;
      }
    }
    return descriptors;
  }
  // No primvars container published: nothing to sync.
  return descriptors;
}

// The flattened value of a primvar (indexed primvars are flattened by the
// primvar schema).
VtValue GetMeshPrimvarValue(HdSceneDelegate* sceneDelegate, const SdfPath& id,
                            const TfToken& name) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdPrimvarSchema primvar =
        HdPrimvarsSchema::GetFromParent(data_source).GetPrimvar(name);
    if (HdSampledDataSourceHandle value = primvar.GetPrimvarValue()) {
      return value->GetValue(0.0f);
    }
    return VtValue();
  }
  return VtValue();
}

// The index array of an indexed primvar (empty for non-indexed primvars).
VtIntArray GetMeshPrimvarIndices(HdSceneDelegate* sceneDelegate,
                                 const SdfPath& id, const TfToken& name) {
  if (HdContainerDataSourceHandle data_source =
          GetTerminalPrimDataSource(sceneDelegate, id)) {
    HdPrimvarSchema primvar =
        HdPrimvarsSchema::GetFromParent(data_source).GetPrimvar(name);
    if (HdIntArrayDataSourceHandle indices = primvar.GetIndices()) {
      return indices->GetTypedValue(0.0f);
    }
    return VtIntArray();
  }
  return VtIntArray();
}

std::optional<SdfPath> GetAttachedSensorId(HdSceneDelegate* sceneDelegate,
                                           const SdfPath& id) {
  VtValue attached_sensor =
      GetCustomPrimValue(sceneDelegate, id, TfToken("mitsuba:sensor"));
  if (attached_sensor.IsHolding<SdfPath>()) {
    return attached_sensor.Get<SdfPath>();
  } else if (attached_sensor.IsHolding<std::string>()) {
    return SdfPath(attached_sensor.Get<std::string>());
  }
  return std::nullopt;
}

std::optional<LightSpec> GetMeshEmitterSpec(HdSceneDelegate* sceneDelegate,
                                            const SdfPath& id) {
  VtValue light_intensity =
      sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity);
  if (light_intensity.IsEmpty() || !light_intensity.IsHolding<float>()) {
    return std::nullopt;
  }
  LightSpec spec;
  spec.id = id;
  float intensity = light_intensity.Get<float>();
  GfVec3f color(1.0f, 1.0f, 1.0f);
  VtValue light_color =
      sceneDelegate->GetLightParamValue(id, HdLightTokens->color);
  if (light_color.IsHolding<GfVec3f>()) {
    color = light_color.Get<GfVec3f>();
  }
  float exposure = 0.0f;
  VtValue light_exposure =
      sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure);
  if (light_exposure.IsHolding<float>()) {
    exposure = light_exposure.Get<float>();
  }
  spec.emission = color * intensity * std::exp2(exposure);
  return spec;
}

}  // namespace

TF_DEFINE_PUBLIC_TOKENS(HdMitsubaMeshTokens, HDMITSUBA_MESH_TOKENS);

HdMitsubaMesh::HdMitsubaMesh(const SdfPath& id, const SdfPath& /*instancerId*/)
    : HdMesh(id) {}

HdDirtyBits HdMitsubaMesh::GetInitialDirtyBits() const {
  return HdChangeTracker::AllDirty;
}

HdDirtyBits HdMitsubaMesh::GetInitialDirtyBitsMask() const {
  return HdChangeTracker::AllDirty;
}

HdDirtyBits HdMitsubaMesh::_PropagateDirtyBits(HdDirtyBits bits) const {
  return bits;
}

void HdMitsubaMesh::Sync(HdSceneDelegate* sceneDelegate,
                         HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
                         const TfToken& /*reprToken*/) {
  TRACE_FUNCTION();
  if (*dirtyBits == HdChangeTracker::Clean) {
    return;
  }
  auto* mitsuba_delegate = dynamic_cast<HdMitsubaRenderDelegate*>(
      sceneDelegate->GetRenderIndex().GetRenderDelegate());
  if (mitsuba_delegate && mitsuba_delegate->NativeClaimed(GetId())) {
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }
  _UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(),
                                        GetInstancerId());
  TF_DEBUG(HDMITSUBA_SYNC).Msg("HdMitsubaMesh::Sync: %s\n", GetId().GetText());

  auto* mitsuba_render_param = static_cast<HdMitsubaRenderParam*>(renderParam);
  auto* scene = mitsuba_render_param->GetScene();

  // Visibility changes can make the mesh visible or invisible.
  bool visible = GetMeshVisibility(sceneDelegate, GetId());
  if ((*dirtyBits & HdChangeTracker::DirtyVisibility) && visible) {
    *dirtyBits |= HdChangeTracker::AllDirty;
  }
  if (!visible) {
    RemoveFromScene(scene);
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }

  // Update topology and/or primvars if the corresponding bits are set.
  bool topology_dirty = *dirtyBits & (HdChangeTracker::DirtyTopology |
                                      HdChangeTracker::DirtyMaterialId |
                                      HdChangeTracker::DirtyDisplayStyle);
  topology_dirty =
      topology_dirty ||
      HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(),
                                      HdMitsubaMeshTokens->subdivision_level);
  bool primvars_dirty =
      *dirtyBits &
      (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyPoints |
       HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyTransform);
  bool instancer_dirty = *dirtyBits & (HdChangeTracker::DirtyInstancer |
                                       HdChangeTracker::DirtyInstanceIndex);

  if (topology_dirty) {
    SyncTopology(sceneDelegate);
  }

  if (topology_dirty || primvars_dirty || instancer_dirty) {
    UpdateScene(sceneDelegate, renderParam,
                SyncPrimvars(sceneDelegate, dirtyBits), dirtyBits);
  }

  *dirtyBits = HdChangeTracker::Clean;
}

void HdMitsubaMesh::Finalize(HdRenderParam* renderParam) {
  auto* mitsuba_render_param = static_cast<HdMitsubaRenderParam*>(renderParam);
  auto* scene = mitsuba_render_param->GetScene();
  RemoveFromScene(scene);
}

void HdMitsubaMesh::_InitRepr(const TfToken& reprToken,
                              HdDirtyBits* dirtyBits) {
  if (reprToken == HdReprTokens->hull) {
    *dirtyBits |=
        HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
        HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyMaterialId;
  }
}

void HdMitsubaMesh::SyncTopology(HdSceneDelegate* sceneDelegate) {
  TRACE_FUNCTION();
  int refineLevel = GetMeshRefineLevel(sceneDelegate, GetId());
  VtValue subdivLevelValue = GetCustomPrimValue(
      sceneDelegate, GetId(), HdMitsubaMeshTokens->subdivision_level);
  if (!subdivLevelValue.IsEmpty() && subdivLevelValue.IsHolding<int>()) {
    refineLevel = subdivLevelValue.Get<int>();
  }
  std::optional<HdMeshTopology> scene_index_topology =
      GetMeshTopologyFromSceneIndex(sceneDelegate, GetId());
  HdMeshTopology topology =
      HdMeshTopology(scene_index_topology.value_or(HdMeshTopology()),
                     refineLevel);

  const HdGeomSubsets& geom_subsets = topology.GetGeomSubsets();
  const int num_coarse_faces = topology.GetNumFaces();
  const SdfPath material_id = GetMeshMaterialId(sceneDelegate, GetId());
  material_ids_.clear();
  material_ids_.push_back(material_id);
  face_material_indices_.assign(num_coarse_faces, 0);
  for (const auto& subset : geom_subsets) {
    auto it = std::find(material_ids_.begin(), material_ids_.end(),
                        subset.materialId);
    int material_index = 0;
    if (it == material_ids_.end()) {
      material_index = material_ids_.size();
      material_ids_.push_back(subset.materialId);
    } else {
      material_index = std::distance(material_ids_.begin(), it);
    }
    for (const int face_index : subset.indices) {
      if (face_index >= 0 && face_index < num_coarse_faces) {
        face_material_indices_[face_index] = material_index;
      }
    }
  }

  const TfToken scheme = topology.GetScheme();
  absl::flat_hash_map<TfToken, int, TfToken::HashFunctor>
      fvar_primvar_to_channel;
  std::vector<VtIntArray> fvar_topologies;
  const TfToken fvar_interp_rule =
      topology.GetSubdivTags().GetFaceVaryingInterpolationRule();

  if (fvar_interp_rule != PxOsdOpenSubdivTokens->all && refineLevel > 0 &&
      (scheme == PxOsdOpenSubdivTokens->catmullClark ||
       scheme == PxOsdOpenSubdivTokens->loop)) {
    for (const auto& [name, primvar] :
         GetAllMeshPrimvarDescriptors(sceneDelegate, GetId())) {
      if (primvar.interpolation != HdInterpolationFaceVarying) {
        continue;
      }
      VtIntArray indices;
      if (primvar.indexed) {
        indices = GetMeshPrimvarIndices(sceneDelegate, GetId(), primvar.name);
      } else {
        VtValue value = GetMeshPrimvarValue(sceneDelegate, GetId(),
                                            primvar.name);
        if (!value.IsEmpty()) {
          const int num_face_varyings = topology.GetNumFaceVaryings();
          indices.resize(num_face_varyings);
          std::iota(indices.begin(), indices.end(), 0);
        }
      }
      if (!indices.empty()) {
        auto it =
            std::find(fvar_topologies.begin(), fvar_topologies.end(), indices);
        int channel;
        if (it == fvar_topologies.end()) {
          channel = fvar_topologies.size();
          fvar_topologies.push_back(indices);
        } else {
          channel = std::distance(fvar_topologies.begin(), it);
        }
        fvar_primvar_to_channel[primvar.name] = channel;
      }
    }
  }

  topology.SetSubdivTags(GetMeshSubdivTags(sceneDelegate, GetId()));

  subdiv_evaluator_.Initialize(topology.GetPxOsdMeshTopology(), refineLevel,
                               scheme, topology.GetSubdivTags(),
                               fvar_topologies, fvar_primvar_to_channel,
                               TfToken(GetId().GetText()));

  if (subdiv_evaluator_.IsSubdivided()) {
    topology_ = HdMeshTopology(scheme, topology.GetOrientation(),
                               subdiv_evaluator_.GetRefinedFaceVertexCounts(),
                               subdiv_evaluator_.GetRefinedFaceVertexIndices());
  } else {
    topology_ = std::move(topology);
  }
}

void HdMitsubaMesh::UpdateScene(HdSceneDelegate* sceneDelegate,
                                HdRenderParam* renderParam,
                                const PrimvarMap& primvars,
                                HdDirtyBits* dirtyBits) {
  TRACE_FUNCTION();
  auto* mitsuba_render_param = static_cast<HdMitsubaRenderParam*>(renderParam);
  auto* scene = mitsuba_render_param->GetScene();
  auto id = GetId();
  auto points_it = primvars.find(HdTokens->points);
  if (points_it == primvars.end() || points_it->second.value.IsEmpty() ||
      points_it->second.value.Get<VtVec3fArray>().empty()) {
    TF_DEBUG(HDMITSUBA_LIFECYCLE)
        .Msg("Mesh %s has no points. Removing from scene.\n", id.GetText());
    RemoveFromScene(scene);
    return;
  }

  std::optional<SdfPath> attached_sensor_id =
      GetAttachedSensorId(sceneDelegate, id);
  std::optional<LightSpec> emitter_spec = GetMeshEmitterSpec(sceneDelegate, id);

  VtMatrix4dArray instance_transforms;
  bool transforms_dirty = false;
  const SdfPath& instancer_id = GetInstancerId();
  if (!instancer_id.IsEmpty()) {
    HdMitsubaInstancer* instancer = static_cast<HdMitsubaInstancer*>(
        sceneDelegate->GetRenderIndex().GetInstancer(instancer_id));
    if (instancer) {
      instance_transforms = instancer->ComputeInstanceTransforms(id);
      transforms_dirty =
          (dirtyBits && (*dirtyBits & HdChangeTracker::DirtyInstancer)) ||
          (dirtyBits && (*dirtyBits & HdChangeTracker::DirtyInstanceIndex));
    }
  }

  bool instance_count_changed = instance_transforms.size() != instance_count_;
  instance_count_ = instance_transforms.size();

  bool topology_dirty =
      dirtyBits && (*dirtyBits & HdChangeTracker::DirtyTopology);
  bool needs_rebuild = topology_dirty || instance_count_changed;

  MeshSpec spec;
  spec.id = id;
  spec.material_ids = material_ids_;
  spec.primvars = primvars;
  spec.transform = GetMeshTransform(sceneDelegate, id);
  spec.attached_sensor_id = attached_sensor_id;
  spec.emitter_spec = emitter_spec;
  spec.instance_transforms = instance_transforms;
  spec.transforms_dirty = transforms_dirty;
  spec.needs_rebuild = needs_rebuild;
  spec.is_subdivided = subdiv_evaluator_.IsSubdivided();

  if (subdiv_evaluator_.IsSubdivided()) {
    const auto& refined_to_coarse = subdiv_evaluator_.GetRefinedToCoarseMap();
    spec.face_material_indices.resize(refined_to_coarse.size());
    for (size_t i = 0; i < refined_to_coarse.size(); ++i) {
      spec.face_material_indices[i] =
          face_material_indices_[refined_to_coarse[i]];
    }
    spec.face_vertex_counts = subdiv_evaluator_.GetRefinedFaceVertexCounts();
    spec.face_vertex_indices = subdiv_evaluator_.GetRefinedFaceVertexIndices();
  } else {
    spec.face_material_indices = face_material_indices_;
    spec.face_vertex_counts = topology_.GetFaceVertexCounts();
    spec.face_vertex_indices = topology_.GetFaceVertexIndices();
  }

  scene->SyncMesh(std::move(spec));
  in_scene_ = true;
}

void HdMitsubaMesh::RemoveFromScene(SceneManager* scene) {
  if (in_scene_) {
    scene->RemoveShape(GetId());
    in_scene_ = false;
  }
}

HdMitsubaMesh::PrimvarMap HdMitsubaMesh::SyncPrimvars(
    HdSceneDelegate* sceneDelegate, HdDirtyBits* dirtyBits) {
  TRACE_FUNCTION();
  TF_DEBUG(HDMITSUBA_SYNC)
      .Msg("SyncPrimvars for %s dirtyBits: %d subdivided: %d\n",
           GetId().GetText(), *dirtyBits, subdiv_evaluator_.IsSubdivided());
  auto primvar_descriptors =
      GetAllMeshPrimvarDescriptors(sceneDelegate, GetId());

  // Remove primvars that no longer exist from the primvar map.
  // Protect built-in geometric attributes (points, normals) from removal.
  if (*dirtyBits & HdChangeTracker::DirtyPrimvar) {
    for (auto it = primvars_.begin(); it != primvars_.end();) {
      if (it->first != HdTokens->points && it->first != HdTokens->normals &&
          !primvar_descriptors.contains(it->first)) {
        it = primvars_.erase(it);
      } else {
        ++it;
      }
    }
  }

  const SdfPath& id = GetId();
  if (subdiv_evaluator_.IsSubdivided()) {
    primvars_.erase(HdTokens->normals);
  }

  bool transform_dirty = *dirtyBits & HdChangeTracker::DirtyTransform;

  // Computed primvars (e.g. UsdSkel skinning) need no special handling here:
  // the HdMitsubaExtComputationPrimvarPruningSceneIndexPlugin scene index —
  // appended for the Mitsuba renderer by the render index itself — evaluates
  // ext computations and presents their outputs as plain primvars, so they
  // arrive through the regular primvar reads below.
  auto resolve_primvar_value = [&](const TfToken& name) -> VtValue {
    return GetMeshPrimvarValue(sceneDelegate, id, name);
  };

  // Helper to sync and optionally refine a primvar
  auto sync_primvar = [&](const TfToken& name, const HdPrimvarDescriptor& desc,
                          bool refine = true) {
    VtValue value = resolve_primvar_value(name);
    if (value.IsEmpty()) return;
    if (refine && subdiv_evaluator_.IsSubdivided()) {
      value =
          subdiv_evaluator_.RefinePrimvar(value, desc.interpolation, desc.name);
    }
    primvars_[name] = {std::move(value), desc};
  };

  // 1. Explicitly sync points (built-in geometric attribute)
  if ((*dirtyBits & HdChangeTracker::DirtyPoints) || transform_dirty) {
    HdPrimvarDescriptor desc{HdTokens->points, HdInterpolationVertex,
                             HdPrimvarRoleTokens->point};
    sync_primvar(HdTokens->points, desc);
  }

  // 2. Explicitly sync normals (only if no subdivision is used).
  if (!subdiv_evaluator_.IsSubdivided() &&
      ((*dirtyBits & HdChangeTracker::DirtyNormals) || transform_dirty)) {
    VtValue value = resolve_primvar_value(HdTokens->normals);
    if (!value.IsEmpty()) {
      HdPrimvarDescriptor desc;
      if (primvar_descriptors.contains(HdTokens->normals)) {
        desc = primvar_descriptors.at(HdTokens->normals);
      } else {
        desc.name = HdTokens->normals;
        desc.interpolation = HdInterpolationVertex;
        desc.role = HdPrimvarRoleTokens->normal;
      }
      size_t vertex_count = 0;
      auto points_it = primvars_.find(HdTokens->points);
      if (points_it != primvars_.end() &&
          points_it->second.value.IsHolding<VtVec3fArray>()) {
        vertex_count = points_it->second.value.Get<VtVec3fArray>().size();
      }
      size_t face_count = topology_.GetNumFaces();
      size_t corner_count = topology_.GetNumFaceVaryings();
      if (ValidatePrimvarSize(value, desc.interpolation, vertex_count,
                              face_count, corner_count)) {
        primvars_[HdTokens->normals] = {std::move(value), desc};
      } else {
        size_t actual_size =
            value.IsHolding<VtVec3fArray>() ? value.Get<VtVec3fArray>().size()
                                            : 0;
        // Only warn if the user explicitly authored a non-empty array with incorrect length (>1)
        if (actual_size > 1) {
          TF_WARN(
              "Ignored invalid normals for %s (size mismatch). "
              "Interpolation: %d, Actual size: %zu, Expected (Vertex: %zu, Face: "
              "%zu, Corner: %zu)",
              id.GetText(), (int)desc.interpolation, actual_size,
              vertex_count, face_count, corner_count);
        }
      }
    }
  }

  // 3. Sync user primvars
  for (auto const& [token, descriptor] : primvar_descriptors) {
    // Skip built-in geometric attributes that are handled explicitly
    if (token == HdTokens->points || token == HdTokens->normals) {
      continue;
    }
    TF_DEBUG(HDMITSUBA_SYNC).Msg("SyncPrimvar: %s\n", token.GetText());
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, token)) {
      sync_primvar(token, descriptor);
    }
  }
  return primvars_;
}

void TranslateMeshPrim(const HdSceneIndexBaseRefPtr& scene_index,
                       const SdfPath& id, HdDirtyBits dirty_bits,
                       MeshTranslationState* /*state*/,
                       SceneManager* scene_manager) {
  HdSceneIndexPrim prim = scene_index->GetPrim(id);
  if (!prim.dataSource) {
    return;
  }

  // 1. Visibility
  static const HdDataSourceLocator visibility_locator(
      HdVisibilitySchema::GetSchemaToken(),
      HdVisibilitySchemaTokens->visibility);
  const bool visible = GetParam<bool>(prim.dataSource, visibility_locator, true);
  if (!visible) {
    scene_manager->RemoveShape(id);
    return;
  }

  MeshSpec spec;
  spec.id = id;
  spec.transform = GetWorldTransform(*scene_index, id);
  spec.dirty_bits = dirty_bits;

  HdMeshSchema mesh_schema = HdMeshSchema::GetFromParent(prim.dataSource);
  if (mesh_schema.IsDefined()) {
    HdMeshTopologySchema top_schema = mesh_schema.GetTopology();
    if (top_schema.IsDefined()) {
      if (HdIntArrayDataSourceHandle counts_ds =
              top_schema.GetFaceVertexCounts()) {
        spec.face_vertex_counts = counts_ds->GetTypedValue(0.0f);
      }
      if (HdIntArrayDataSourceHandle indices_ds =
              top_schema.GetFaceVertexIndices()) {
        spec.face_vertex_indices = indices_ds->GetTypedValue(0.0f);
      }
    }
    if (HdTokenDataSourceHandle scheme_ds =
            mesh_schema.GetSubdivisionScheme()) {
      TfToken scheme = scheme_ds->GetTypedValue(0.0f);
      spec.is_subdivided =
          (scheme == PxOsdOpenSubdivTokens->catmullClark ||
           scheme == PxOsdOpenSubdivTokens->loop);
    }
  }

  SdfPath mat_path = GetBoundMaterial(*scene_index, id);
  if (!mat_path.IsEmpty()) {
    spec.material_ids.push_back(mat_path);
  }

  ExtractPrimvars(prim.dataSource, spec.primvars);

  HdLightSchema light_schema = HdLightSchema::GetFromParent(prim.dataSource);
  if (light_schema.IsDefined()) {
    LightSpec emitter_spec;
    emitter_spec.id = id;
    emitter_spec.prim_type = prim.primType;
    emitter_spec.transform = UsdToMitsubaTransform(spec.transform);

    GfVec3f color(1.0f, 1.0f, 1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f;

    auto color_it = spec.primvars.find(TfToken("inputs:color"));
    if (color_it != spec.primvars.end() && color_it->second.value.IsHolding<GfVec3f>()) {
      color = color_it->second.value.Get<GfVec3f>();
    }
    auto intensity_it = spec.primvars.find(TfToken("inputs:intensity"));
    if (intensity_it != spec.primvars.end() &&
        intensity_it->second.value.IsHolding<float>()) {
      intensity = intensity_it->second.value.Get<float>();
    }
    auto exposure_it = spec.primvars.find(TfToken("inputs:exposure"));
    if (exposure_it != spec.primvars.end() &&
        exposure_it->second.value.IsHolding<float>()) {
      exposure = exposure_it->second.value.Get<float>();
    }

    float total_intensity = intensity * std::pow(2.0f, exposure);
    emitter_spec.emission = GfVec3f(color[0] * total_intensity,
                                    color[1] * total_intensity,
                                    color[2] * total_intensity);
    spec.emitter_spec = emitter_spec;
  }

  scene_manager->SyncMesh(std::move(spec));
}

PXR_NAMESPACE_CLOSE_SCOPE
