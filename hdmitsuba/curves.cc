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

#include "hdmitsuba/curves.h"
#include "hdmitsuba/debug_codes.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/basisCurves.h>
#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopology.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include "hdmitsuba/prim_translation.h"
#include "hdmitsuba/render_delegate.h"
#include "hdmitsuba/render_param.h"
#include "hdmitsuba/scene_manager.h"
#include "hdmitsuba/spec_types.h"
#include "hdmitsuba/utils.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

float CalculateMeanWidth(const VtFloatArray& widths,
                         float default_radius = 0.01f) {
  if (widths.empty()) return default_radius;
  double mean = 0.0;
  for (size_t i = 0; i < widths.size(); ++i) {
    mean += (widths[i] - mean) / (i + 1);
  }
  return static_cast<float>(mean);
}

template <typename WidthFn>
std::vector<float> PackControlPoints(const VtVec3fArray& points,
                                     WidthFn&& width_fn) {
  std::vector<float> control_points;
  control_points.resize(points.size() * 4);
  for (size_t i = 0; i < points.size(); ++i) {
    const GfVec3f& point = points[i];
    control_points[4 * i + 0] = point[0];
    control_points[4 * i + 1] = point[1];
    control_points[4 * i + 2] = point[2];
    control_points[4 * i + 3] = width_fn(i);
  }
  return control_points;
}

}  // namespace

HdMitsubaCurves::HdMitsubaCurves(const SdfPath& id) : HdBasisCurves(id) {}

HdDirtyBits HdMitsubaCurves::GetInitialDirtyBits() const {
  return GetInitialDirtyBitsMask();
}

HdDirtyBits HdMitsubaCurves::GetInitialDirtyBitsMask() const {
  return HdChangeTracker::Clean | HdChangeTracker::DirtyPrimvar |
         HdChangeTracker::DirtyRepr | HdChangeTracker::DirtyTopology |
         HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
         HdChangeTracker::DirtyMaterialId;
}

void HdMitsubaCurves::Sync(HdSceneDelegate* sceneDelegate,
                           HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
                           const TfToken& /*reprToken*/) {
  auto* mitsuba_delegate = dynamic_cast<HdMitsubaRenderDelegate*>(
      sceneDelegate->GetRenderIndex().GetRenderDelegate());
  if (mitsuba_delegate && mitsuba_delegate->NativeClaimed(GetId())) {
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }
  static const HdDataSourceLocator points_locator(
      HdPrimvarsSchema::GetSchemaToken(), HdTokens->points,
      HdPrimvarSchemaTokens->primvarValue);
  static const HdDataSourceLocator widths_locator(
      HdPrimvarsSchema::GetSchemaToken(), HdTokens->widths,
      HdPrimvarSchemaTokens->primvarValue);
  static const HdDataSourceLocator transform_locator(
      HdXformSchema::GetSchemaToken(), HdXformSchemaTokens->matrix);
  static const HdDataSourceLocator material_locator(
      HdMaterialBindingsSchema::GetSchemaToken(),
      HdMaterialBindingsSchemaTokens->allPurpose,
      HdMaterialBindingSchemaTokens->path);
  static const HdDataSourceLocator visibility_locator(
      HdVisibilitySchema::GetSchemaToken(),
      HdVisibilitySchemaTokens->visibility);
  static const HdDataSourceLocator basis_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->basis);
  static const HdDataSourceLocator type_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->type);
  static const HdDataSourceLocator vertex_counts_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->curveVertexCounts);

  if (*dirtyBits == HdChangeTracker::Clean) return;

  const SdfPath& id = GetId();
  HdSceneIndexBaseRefPtr scene_index =
      sceneDelegate->GetRenderIndex().GetTerminalSceneIndex();
  if (!TF_VERIFY(scene_index)) {
    return;
  }
  HdContainerDataSourceHandle data_source = scene_index->GetPrim(id).dataSource;

  // 1. Visibility
  bool visible = GetParam<bool>(data_source, visibility_locator, true);
  if ((*dirtyBits & HdChangeTracker::DirtyVisibility) && visible) {
    *dirtyBits |= HdChangeTracker::AllDirty;
  }
  SceneManager* scene_manager =
      static_cast<HdMitsubaRenderParam*>(renderParam)->GetScene();

  if (!visible) {
    RemoveFromScene(scene_manager);
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }

  // 2. Transform
  GfMatrix4d transform =
      GetParam<GfMatrix4d>(data_source, transform_locator, GfMatrix4d(1.0));

  // 3. Primvars (Points and Widths)
  if (points_.empty() || widths_.empty() ||
      HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points) ||
      HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->widths)) {
    points_ = GetParam<VtVec3fArray>(data_source, points_locator);
    if (points_.empty()) {
      TF_WARN("Curves %s: missing or invalid points primvar.", id.GetText());
    }
    widths_ = GetParam<VtFloatArray>(data_source, widths_locator);
    if (widths_.empty()) {
      TF_WARN("Curves %s: missing or invalid widths primvar.", id.GetText());
    }
  }

  // 4. Topology
  std::string plugin_name = "linearcurve";
  TfToken basis = GetParam<TfToken>(data_source, basis_locator);
  TfToken type = GetParam<TfToken>(data_source, type_locator);
  if (basis == TfToken("bspline") && type == TfToken("cubic")) {
    plugin_name = "bsplinecurve";
  }
  VtIntArray vertex_counts =
      GetParam<VtIntArray>(data_source, vertex_counts_locator);

  if (points_.empty() || widths_.empty()) {
    TF_DEBUG(HDMITSUBA_LIFECYCLE)
        .Msg("Curves %s has no points or widths. Removing from scene.\n",
             id.GetText());
    RemoveFromScene(scene_manager);
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }

  // 5. Material binding
  SdfPath material_id = GetParam<SdfPath>(data_source, material_locator);

  // 6. Convert data to CurveSpec and pass it to the scene manager.
  CurveSpec spec;
  spec.id = id;
  spec.transform = UsdToMitsubaTransform(transform);
  spec.material_id = material_id;
  spec.needs_rebuild = true;
  spec.plugin_name = plugin_name;

  // Pack control points [x, y, z, r] (lazy-evaluating widths based on layout)
  if (widths_.size() == points_.size()) {
    spec.control_points =
        PackControlPoints(points_, [&](size_t i) { return widths_[i] * 0.5f; });
  } else if (widths_.size() == 1) {
    float r = widths_[0] * 0.5f;
    spec.control_points =
        PackControlPoints(points_, [&](size_t /*i*/) { return r; });
  } else {
    float uniform_radius = CalculateMeanWidth(widths_) * 0.5f;
    spec.control_points = PackControlPoints(
        points_, [&](size_t /*i*/) { return uniform_radius; });
  }

  // Generate segment indices
  int degree = (plugin_name == "bsplinecurve") ? 3 : 1;
  int n_indices = 0;
  for (int count : vertex_counts) {
    n_indices += std::max(0, count - degree);
  }
  spec.segment_indices.resize(n_indices);
  int index = 0;
  int accumulated_vertices = 0;
  for (int count : vertex_counts) {
    int num_segments = count - degree;
    for (int j = 0; j < num_segments; ++j, ++index) {
      spec.segment_indices[index] = accumulated_vertices + j;
    }
    accumulated_vertices += count;
  }
  scene_manager->SyncCurves(std::move(spec));
  in_scene_ = true;
  *dirtyBits = HdChangeTracker::Clean;
}

void HdMitsubaCurves::Finalize(HdRenderParam* renderParam) {
  RemoveFromScene(static_cast<HdMitsubaRenderParam*>(renderParam)->GetScene());
}

void HdMitsubaCurves::RemoveFromScene(SceneManager* scene) {
  if (in_scene_) {
    scene->RemoveShape(GetId());
    in_scene_ = false;
  }
}

HdDirtyBits HdMitsubaCurves::_PropagateDirtyBits(HdDirtyBits bits) const {
  return bits;
}

void HdMitsubaCurves::_InitRepr(const TfToken& /*reprToken*/,
                                HdDirtyBits* /*dirtyBits*/) {}

void TranslateCurvesPrim(const HdSceneIndexBaseRefPtr& scene_index,
                         const SdfPath& id, HdDirtyBits dirty_bits,
                         CurvesTranslationState* /*state*/,
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

  CurveSpec spec;
  spec.id = id;
  GfMatrix4d m = GetWorldTransform(*scene_index, id);
  spec.transform = UsdToMitsubaTransform(m);
  spec.material_id = GetBoundMaterial(*scene_index, id);
  spec.dirty_bits = dirty_bits;

  // Primvars
  PrimvarMap pvars;
  ExtractPrimvars(prim.dataSource, pvars);
  VtVec3fArray points;
  auto points_it = pvars.find(HdTokens->points);
  if (points_it != pvars.end() &&
      points_it->second.value.IsHolding<VtVec3fArray>()) {
    points = points_it->second.value.Get<VtVec3fArray>();
  }

  VtFloatArray widths;
  auto widths_it = pvars.find(HdTokens->widths);
  if (widths_it != pvars.end() &&
      widths_it->second.value.IsHolding<VtFloatArray>()) {
    widths = widths_it->second.value.Get<VtFloatArray>();
  }

  if (widths.size() == points.size()) {
    spec.control_points =
        PackControlPoints(points, [&](size_t i) { return widths[i]; });
  } else {
    float radius = CalculateMeanWidth(widths);
    spec.control_points =
        PackControlPoints(points, [&](size_t /*i*/) { return radius; });
  }

  // Basis / Topology
  static const HdDataSourceLocator basis_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->basis);
  static const HdDataSourceLocator type_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->type);
  static const HdDataSourceLocator vertex_counts_locator(
      TfToken("basisCurves"), TfToken("topology"),
      HdBasisCurvesTopologySchemaTokens->curveVertexCounts);

  TfToken curve_type =
      GetParam<TfToken>(prim.dataSource, type_locator, HdTokens->cubic);
  TfToken curve_basis =
      GetParam<TfToken>(prim.dataSource, basis_locator, HdTokens->bezier);
  VtIntArray curve_vertex_counts = GetParam<VtIntArray>(
      prim.dataSource, vertex_counts_locator, VtIntArray());

  if (curve_type == HdTokens->linear) {
    spec.plugin_name = "linearcurve";
  } else if (curve_type == HdTokens->cubic) {
    if (curve_basis == HdTokens->bezier) {
      spec.plugin_name = "beziercurve";
    } else if (curve_basis == HdTokens->bspline) {
      spec.plugin_name = "bsplinecurve";
    } else {
      TF_WARN("Unsupported curve basis %s for %s, falling back to linearcurve",
              curve_basis.GetText(), id.GetText());
      spec.plugin_name = "linearcurve";
    }
  } else {
    spec.plugin_name = "linearcurve";
  }

  uint32_t total_segments = 0;
  for (int count : curve_vertex_counts) {
    total_segments += std::max(0, count - 1);
  }
  spec.segment_indices.resize(total_segments);
  uint32_t accumulated_vertices = 0;
  uint32_t segment_index = 0;
  for (int count : curve_vertex_counts) {
    int segment_count = std::max(0, count - 1);
    for (int j = 0; j < segment_count; ++j) {
      uint32_t index = segment_index++;
      spec.segment_indices[index] = accumulated_vertices + j;
    }
    accumulated_vertices += count;
  }

  scene_manager->SyncCurves(std::move(spec));
}

PXR_NAMESPACE_CLOSE_SCOPE
