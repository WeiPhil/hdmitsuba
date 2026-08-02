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

#ifndef HDMITSUBA_PRIM_TRANSLATION_H_
#define HDMITSUBA_PRIM_TRANSLATION_H_

// Scene-index-native prim translation: turns a prim's data source on the
// terminal scene index into a hdMitsuba spec and pushes it into the scene
// manager. Shared by the Hydra prims (whose Sync methods wrap these with
// their emulated dirty-bit bookkeeping) and the native scene index backend
// (which calls them directly from change notifications, with no prim sync
// involved). Per-prim translation state that used to live on the prim
// objects lives in the *TranslationState structs so both callers can own it.

#include <string>

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_OPEN_SCOPE

class SceneManager;

struct MaterialTranslationState {
  HdMaterialNetwork2 last_network;
  bool has_last_network = false;
};

struct LightTranslationState {
  bool is_instantiated = false;
  bool treat_as_point = false;
  float shaping_cone_angle = 0.0f;
  std::string texture_file_path;
};

struct CameraTranslationState {
  bool is_instantiated = false;
  std::string sensor_type;
  std::string film_pixel_filter_type;
};

// Translates the material prim at `id` and syncs it into `scene_manager`.
void TranslateMaterialPrim(const HdSceneIndexBaseRefPtr& scene_index,
                           const SdfPath& id, MaterialTranslationState* state,
                           SceneManager* scene_manager);

// Translates the light prim at `id` (of Hydra prim type `prim_type`).
// `dirty_bits` is forwarded into the spec for the scene manager's
// update-vs-rebuild decisions; native callers pass HdLight::AllDirty.
void TranslateLightPrim(const HdSceneIndexBaseRefPtr& scene_index,
                        const SdfPath& id, const TfToken& prim_type,
                        HdDirtyBits dirty_bits, LightTranslationState* state,
                        SceneManager* scene_manager);

// Translates the camera prim at `id` from its camera/xform schemas.
// `dirty_bits` is forwarded into the spec; native callers pass
// HdCamera::AllDirty.
void TranslateCameraPrim(const HdSceneIndexBaseRefPtr& scene_index,
                         const SdfPath& id, HdDirtyBits dirty_bits,
                         CameraTranslationState* state,
                         SceneManager* scene_manager);

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // HDMITSUBA_PRIM_TRANSLATION_H_
