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

#include "hdmitsuba/camera.h"

#include "hdmitsuba/prim_translation.h"
#include "hdmitsuba/render_delegate.h"

#include <string>
#include <utility>

#include <drjit/math.h>
#include <drjit/sphere.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include "hdmitsuba/debug_codes.h"
#include "hdmitsuba/render_param.h"
#include "hdmitsuba/spec_types.h"
#include "hdmitsuba/utils.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace dr = drjit;

namespace {

ScalarAffineTransform4f RemoveScaleFromTransform(
    const ScalarAffineTransform4f& transform) {
  auto [s, q, t] = dr::transform_decompose(transform.matrix);
  return ScalarAffineTransform4f(dr::transform_compose<dr::Matrix<float, 4>>(
      dr::Matrix<float, 3>(1.0f), q, t));
}

ScalarAffineTransform4f UsdToMitsubaSensorTransform(
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

// Reads a custom (namespaced) camera parameter.
//
// Custom `mitsuba:*` camera attributes are overlaid into the prim's `camera`
// container by the keyless UsdImagingMitsubaAttributesAdapter, so they are
// read from the render index's terminal scene index; an unauthored attribute
// is an empty value.
VtValue GetCustomCameraParamValue(HdSceneDelegate* sceneDelegate,
                                  const SdfPath& id, const TfToken& key) {
  if (HdSceneIndexBaseRefPtr scene_index =
          sceneDelegate->GetRenderIndex().GetTerminalSceneIndex()) {
    if (HdContainerDataSourceHandle prim_source =
            scene_index->GetPrim(id).dataSource) {
      if (HdContainerDataSourceHandle camera_source =
              HdContainerDataSource::Cast(
                  prim_source->Get(HdCameraSchema::GetSchemaToken()))) {
        if (HdSampledDataSourceHandle sampled =
                HdSampledDataSource::Cast(camera_source->Get(key))) {
          VtValue value = sampled->GetValue(0.0f);
          if (!value.IsEmpty()) {
            return value;
          }
        }
      }
    }
  }
  return VtValue();
}

}  // namespace

HdMitsubaCamera::HdMitsubaCamera(const SdfPath& id) : HdCamera(id) {}

void HdMitsubaCamera::Sync(HdSceneDelegate* sceneDelegate,
                           HdRenderParam* renderParam, HdDirtyBits* dirtyBits) {
  if (static_cast<HdMitsubaRenderDelegate*>(
          sceneDelegate->GetRenderIndex().GetRenderDelegate())
          ->NativeClaimed(GetId())) {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    *dirtyBits = HdChangeTracker::Clean;
    return;
  }
  HdDirtyBits dirty_bits_copy = *dirtyBits;
  TF_DEBUG(HDMITSUBA_SYNC)
      .Msg("HdMitsubaCamera::Sync: %s\n", GetId().GetText());
  HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);  // Clears dirty bits.

  std::string sensor_type =
      GetCustomCameraParamValue(sceneDelegate, GetId(),
                                TfToken("mitsuba:sensor:type"))
          .GetWithDefault<std::string>("perspective");
  if (dirty_bits_copy & HdCamera::DirtyParams) {
    film_pixel_filter_type_ =
        GetCustomCameraParamValue(
            sceneDelegate, GetId(),
            TfToken("mitsuba:sensor:film:pixel_filter:type"))
            .GetWithDefault<std::string>("");
  }

  CameraSpec spec;
  spec.id = GetId();
  spec.transform = UsdToMitsubaSensorTransform(GetTransform());
  spec.sensor_type = sensor_type;
  spec.fov = GetHorizontalFieldOfView();
  spec.horizontal_aperture_offset = GetHorizontalPrincipalPointOffset();
  spec.vertical_aperture_offset = GetVerticalPrincipalPointOffset();
  spec.pixel_filter_type = film_pixel_filter_type_;
  spec.near_clip = GetClippingRange().GetMin();
  spec.far_clip = GetClippingRange().GetMax();
  spec.dirty_bits = dirty_bits_copy;

  spec.needs_rebuild = !is_instantiated_;
  spec.needs_rebuild |= sensor_type != sensor_type_;
  spec.needs_rebuild |= (sensor_type != "perspective" &&
                         (dirty_bits_copy & HdCamera::DirtyParams));
  is_instantiated_ = true;
  sensor_type_ = sensor_type;

  static_cast<HdMitsubaRenderParam*>(renderParam)
      ->GetScene()
      ->SyncCamera(std::move(spec));
}


namespace {

template <typename T>
T GetCameraField(const HdContainerDataSourceHandle& camera_source,
                 const TfToken& key, const T& fallback) {
  if (!camera_source) return fallback;
  if (auto typed = HdTypedSampledDataSource<T>::Cast(camera_source->Get(key))) {
    return typed->GetTypedValue(0.0f);
  }
  return fallback;
}

}  // namespace

// Scene-index-native camera translation: reads the camera and xform schemas
// directly, mirroring what HdCamera::Sync + the Sync above assemble through
// the emulated path.
void TranslateCameraPrim(const HdSceneIndexBaseRefPtr& scene_index,
                         const SdfPath& id, HdDirtyBits dirty_bits,
                         CameraTranslationState* state,
                         SceneManager* scene_manager) {
  HdContainerDataSourceHandle prim_source = scene_index->GetPrim(id).dataSource;
  if (!prim_source) {
    return;
  }
  HdContainerDataSourceHandle camera_source = HdContainerDataSource::Cast(
      prim_source->Get(HdCameraSchema::GetSchemaToken()));

  GfMatrix4d transform(1.0);
  HdXformSchema xform = HdXformSchema::GetFromParent(prim_source);
  if (xform.IsDefined()) {
    if (HdMatrixDataSourceHandle matrix = xform.GetMatrix()) {
      transform = matrix->GetTypedValue(0.0f);
    }
  }

  const float focal_length =
      GetCameraField<float>(camera_source, HdCameraSchemaTokens->focalLength,
                            0.0f);
  const float horizontal_aperture = GetCameraField<float>(
      camera_source, HdCameraSchemaTokens->horizontalAperture, 0.0f);
  const float vertical_aperture = GetCameraField<float>(
      camera_source, HdCameraSchemaTokens->verticalAperture, 0.0f);
  const float horizontal_aperture_offset = GetCameraField<float>(
      camera_source, HdCameraSchemaTokens->horizontalApertureOffset, 0.0f);
  const float vertical_aperture_offset = GetCameraField<float>(
      camera_source, HdCameraSchemaTokens->verticalApertureOffset, 0.0f);
  GfVec2f clipping_range(0.01f, 1000.0f);
  if (camera_source) {
    if (auto typed = HdTypedSampledDataSource<GfVec2f>::Cast(
            camera_source->Get(HdCameraSchemaTokens->clippingRange))) {
      clipping_range = typed->GetTypedValue(0.0f);
    }
  }

  auto get_custom = [&](const TfToken& key) -> VtValue {
    if (!camera_source) return VtValue();
    if (auto sampled = HdSampledDataSource::Cast(camera_source->Get(key))) {
      return sampled->GetValue(0.0f);
    }
    return VtValue();
  };

  std::string sensor_type =
      get_custom(TfToken("mitsuba:sensor:type"))
          .GetWithDefault<std::string>("perspective");
  VtValue filter_value =
      get_custom(TfToken("mitsuba:sensor:film:pixel_filter:type"));
  if (!filter_value.IsEmpty()) {
    state->film_pixel_filter_type =
        filter_value.GetWithDefault<std::string>("");
  }

  CameraSpec spec;
  spec.id = id;
  spec.transform = UsdToMitsubaSensorTransform(transform);
  spec.sensor_type = sensor_type;
  if (focal_length > 0.0f && horizontal_aperture > 0.0f) {
    spec.fov = dr::rad_to_deg(
        2.0 * dr::atan(horizontal_aperture / (2.0 * focal_length)));
  } else {
    spec.fov = 0.0f;
  }
  spec.horizontal_aperture_offset =
      horizontal_aperture > 0.0f
          ? horizontal_aperture_offset / horizontal_aperture
          : 0.0f;
  spec.vertical_aperture_offset =
      vertical_aperture > 0.0f
          ? -vertical_aperture_offset / vertical_aperture
          : 0.0f;
  spec.pixel_filter_type = state->film_pixel_filter_type;
  spec.near_clip = clipping_range[0];
  spec.far_clip = clipping_range[1];
  spec.dirty_bits = dirty_bits;

  spec.needs_rebuild = !state->is_instantiated;
  spec.needs_rebuild |= sensor_type != state->sensor_type;
  spec.needs_rebuild |= (sensor_type != "perspective");
  state->is_instantiated = true;
  state->sensor_type = sensor_type;

  scene_manager->SyncCamera(std::move(spec));
}

float HdMitsubaCamera::GetHorizontalFieldOfView() const {
  if (_focalLength <= 0.0f || _horizontalAperture <= 0.0f) return 0.0f;
  return dr::rad_to_deg(2.0 *
                        dr::atan(_horizontalAperture / (2.0 * _focalLength)));
}

float HdMitsubaCamera::GetHorizontalPrincipalPointOffset() const {
  if (_horizontalAperture <= 0.0f) return 0.0f;
  return _horizontalApertureOffset / _horizontalAperture;
}

float HdMitsubaCamera::GetVerticalPrincipalPointOffset() const {
  if (_verticalAperture <= 0.0f) return 0.0f;
  return -_verticalApertureOffset / _verticalAperture;
}

PXR_NAMESPACE_CLOSE_SCOPE
