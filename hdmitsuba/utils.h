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

#pragma once

#include <drjit/matrix.h>
#include <drjit/sphere.h>
#include <mitsuba/core/transform.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/type.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include "hdmitsuba/mesh/geometry_processor.h"

PXR_NAMESPACE_OPEN_SCOPE

template <typename T>
T GetParam(const HdContainerDataSourceHandle& container, const TfToken& name,
           const T& default_value = T()) {
  if (!container) return default_value;
  if (auto data_source = HdSampledDataSource::Cast(container->Get(name))) {
    VtValue value = data_source->GetValue(0.0f);
    if (value.IsHolding<T>()) {
      return value.UncheckedGet<T>();
    } else {
      TF_WARN("GetParam type mismatch for '%s': expected %s, got %s (empty=%d)",
              name.GetText(), TfType::Find<T>().GetTypeName().c_str(),
              value.GetTypeName().c_str(), value.IsEmpty());
    }
  }
  return default_value;
}

template <typename T>
T GetParam(const HdContainerDataSourceHandle& container,
           const HdDataSourceLocator& locator, const T& default_value = T()) {
  if (!container) return default_value;
  if (auto data_source = HdSampledDataSource::Cast(
          HdContainerDataSource::Get(container, locator))) {
    VtValue value = data_source->GetValue(0.0f);
    if (value.IsHolding<T>()) {
      return value.UncheckedGet<T>();
    } else {
      TF_WARN(
          "GetParam type mismatch for locator '%s': expected %s, got %s "
          "(empty=%d)",
          locator.GetString().c_str(), TfType::Find<T>().GetTypeName().c_str(),
          value.GetTypeName().c_str(), value.IsEmpty());
    }
  }
  return default_value;
}

using ScalarAffineTransform4f =
    mitsuba::Transform<mitsuba::Point<float, 4>, true>;

inline ScalarAffineTransform4f UsdToMitsubaTransform(const GfMatrix4d& transform) {
  const double* m = transform.GetArray();
  drjit::Matrix<float, 4> to_world_mat(
      static_cast<float>(m[0]), static_cast<float>(m[4]), static_cast<float>(m[8]), static_cast<float>(m[12]),
      static_cast<float>(m[1]), static_cast<float>(m[5]), static_cast<float>(m[9]), static_cast<float>(m[13]),
      static_cast<float>(m[2]), static_cast<float>(m[6]), static_cast<float>(m[10]), static_cast<float>(m[14]),
      static_cast<float>(m[3]), static_cast<float>(m[7]), static_cast<float>(m[11]), static_cast<float>(m[15]));
  return ScalarAffineTransform4f(to_world_mat);
}

inline ScalarAffineTransform4f RemoveScaleFromTransform(
    const ScalarAffineTransform4f& transform) {
  auto [s, q, t] = drjit::transform_decompose(transform.matrix);
  return ScalarAffineTransform4f(drjit::transform_compose<drjit::Matrix<float, 4>>(
      drjit::Matrix<float, 3>(1.0f), q, t));
}

inline ScalarAffineTransform4f UsdToMitsubaSensorTransform(
    const GfMatrix4d& transform) {
  ScalarAffineTransform4f to_world = UsdToMitsubaTransform(transform);
  if (to_world.has_scale()) {
    to_world = RemoveScaleFromTransform(to_world);
  }
  using ScalarVector3f = mitsuba::Vector<float, 3>;
  to_world =
      to_world * ScalarAffineTransform4f::rotate(ScalarVector3f(0, 1, 0), 180);
  return to_world;
}

inline GfMatrix4d GetWorldTransform(const HdSceneIndexBase& scene_index,
                                    const SdfPath& prim_path) {
  if (prim_path.IsEmpty()) {
    return GfMatrix4d(1.0);
  }
  GfMatrix4d world_matrix(1.0);
  SdfPathVector prefixes;
  prim_path.GetPrefixes(&prefixes);
  for (const SdfPath& prefix : prefixes) {
    HdSceneIndexPrim prim = scene_index.GetPrim(prefix);
    if (prim.dataSource) {
      HdXformSchema xform = HdXformSchema::GetFromParent(prim.dataSource);
      if (xform.IsDefined()) {
        if (HdMatrixDataSourceHandle matrix = xform.GetMatrix()) {
          GfMatrix4d local_m = matrix->GetTypedValue(0.0f);
          world_matrix = local_m * world_matrix;
        }
      }
    }
  }
  return world_matrix;
}

inline SdfPath GetBoundMaterial(const HdSceneIndexBase& scene_index,
                                const SdfPath& prim_path) {
  static const TfToken kDirectMaterialBinding("directMaterialBinding");
  static const TfToken kMaterialPath("materialPath");
  static const TfToken kPath("path");

  SdfPath curr = prim_path;
  while (!curr.IsEmpty() && curr != SdfPath::AbsoluteRootPath()) {
    HdSceneIndexPrim prim = scene_index.GetPrim(curr);
    if (prim.dataSource) {
      for (const TfToken& key :
           {TfToken("materialBindings"), TfToken("usdMaterialBindings")}) {
        if (auto c_ds = HdContainerDataSource::Cast(prim.dataSource->Get(key))) {
          for (const TfToken& purpose : c_ds->GetNames()) {
            if (auto purpose_ds = c_ds->Get(purpose)) {
              auto check_container = [](const HdContainerDataSourceHandle& c) -> SdfPath {
                if (!c) return SdfPath();
                if (auto direct_c = HdContainerDataSource::Cast(c->Get(kDirectMaterialBinding))) {
                  if (auto path_ds = HdPathDataSource::Cast(direct_c->Get(kMaterialPath))) {
                    return path_ds->GetTypedValue(0.0f);
                  }
                  if (auto path_ds = HdPathDataSource::Cast(direct_c->Get(kPath))) {
                    return path_ds->GetTypedValue(0.0f);
                  }
                }
                HdMaterialBindingSchema binding(c);
                if (binding.IsDefined()) {
                  if (HdPathDataSourceHandle path_ds = binding.GetPath()) {
                    return path_ds->GetTypedValue(0.0f);
                  }
                }
                return SdfPath();
              };

              if (auto vec_ds = HdVectorDataSource::Cast(purpose_ds)) {
                for (size_t i = 0; i < vec_ds->GetNumElements(); ++i) {
                  SdfPath mat_path = check_container(HdContainerDataSource::Cast(vec_ds->GetElement(i)));
                  if (!mat_path.IsEmpty()) return mat_path;
                }
              } else if (auto elem_ds = HdContainerDataSource::Cast(purpose_ds)) {
                SdfPath mat_path = check_container(elem_ds);
                if (!mat_path.IsEmpty()) return mat_path;
              }
            }
          }
        }
      }
    }
    curr = curr.GetParentPath();
  }
  return SdfPath();
}

inline void ExtractPrimvars(const HdContainerDataSourceHandle& prim_source,
                            PrimvarMap& out_primvars) {
  HdPrimvarsSchema primvars_schema =
      HdPrimvarsSchema::GetFromParent(prim_source);
  if (!primvars_schema.IsDefined()) {
    return;
  }
  TfTokenVector names = primvars_schema.GetContainer()->GetNames();
  for (const TfToken& name : names) {
    HdPrimvarSchema pv_schema = primvars_schema.GetPrimvar(name);
    if (!pv_schema.IsDefined()) {
      continue;
    }
    PrimvarState pv_state;
    if (HdTokenDataSourceHandle interp_ds = pv_schema.GetInterpolation()) {
      TfToken interp_token = interp_ds->GetTypedValue(0.0f);
      if (interp_token == HdPrimvarSchemaTokens->vertex) {
        pv_state.descriptor.interpolation = HdInterpolationVertex;
      } else if (interp_token == HdPrimvarSchemaTokens->uniform) {
        pv_state.descriptor.interpolation = HdInterpolationUniform;
      } else if (interp_token == HdPrimvarSchemaTokens->faceVarying) {
        pv_state.descriptor.interpolation = HdInterpolationFaceVarying;
      } else if (interp_token == HdPrimvarSchemaTokens->varying) {
        pv_state.descriptor.interpolation = HdInterpolationVarying;
      } else if (interp_token == HdPrimvarSchemaTokens->constant) {
        pv_state.descriptor.interpolation = HdInterpolationConstant;
      }
    }
    if (HdSampledDataSourceHandle val_ds =
            HdSampledDataSource::Cast(pv_schema.GetPrimvarValue())) {
      pv_state.value = val_ds->GetValue(0.0f);
    }
    out_primvars[name] = pv_state;
  }
}

PXR_NAMESPACE_CLOSE_SCOPE
