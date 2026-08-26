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

#include <array>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <mitsuba/core/fwd.h>
#include <mitsuba/core/object.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/render/fwd.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/pxr.h>

#include "hdmitsuba/spec_types.h"

PXR_NAMESPACE_OPEN_SCOPE

struct PropertiesHash {
  size_t operator()(const mitsuba::Properties& p) const { return p.hash(); }
};

struct PropertiesEqual {
  bool operator()(const mitsuba::Properties& a,
                  const mitsuba::Properties& b) const {
    return a == b;
  }
};

using TextureCache =
    absl::flat_hash_map<mitsuba::Properties, mitsuba::ref<mitsuba::Object>,
                        PropertiesHash, PropertiesEqual>;

bool UseRawBitmap(const TfToken& source_color_space, const TfToken& input_name);

void SetMitsubaPropertyFromValue(mitsuba::Properties& props,
                                 std::string_view name, const pxr::VtValue& val,
                                 bool invert_float = false);

// Translates a hierarchical HdContainerDataSource into mitsuba::Properties.
// Recursively traverses child containers and populates nested properties.
// If variant is provided and a child container defines a plugin type, the child
// plugin object is instantiated and attached.
mitsuba::Properties ContainerToMitsubaProperties(
    const HdContainerDataSourceHandle& container,
    std::string_view default_plugin_type = "",
    std::string_view variant = "");

// Instantiates a Mitsuba plugin Object from a Properties instance.
mitsuba::ref<mitsuba::Object> BuildPluginFromProperties(
    const mitsuba::Properties& props,
    std::string_view variant,
    mitsuba::ObjectType expected_type = mitsuba::ObjectType::Unknown);

// Recursively translates a hierarchical HdContainerDataSource into an instantiated Mitsuba plugin Object.
mitsuba::ref<mitsuba::Object> BuildPluginFromContainer(
    const HdContainerDataSourceHandle& container,
    std::string_view variant,
    std::string_view default_plugin_type = "",
    mitsuba::ObjectType expected_type = mitsuba::ObjectType::Unknown);

// Template convenience helper for typed objects (Integrator, BSDF, Texture, Sensor, etc.)
template <typename T>
mitsuba::ref<T> BuildPluginFromContainer(
    const HdContainerDataSourceHandle& container,
    std::string_view default_plugin_type = "") {
  return mitsuba::ref<T>(static_cast<T*>(
      BuildPluginFromContainer(container, T::Variant, default_plugin_type, T::Type).get()));
}

// Unflattens a container with delimited keys (e.g. "mitsuba:integrator:max_depth")
// into a truly nested HdContainerDataSource hierarchy.
HdContainerDataSourceHandle UnflattenContainer(
    const HdContainerDataSourceHandle& container,
    char delimiter = ':');

std::optional<mitsuba::Properties> ExtractTextureProperties(
    const std::map<TfToken, pxr::VtValue>& parameters,
    const TfToken& nodeTypeId, const TfToken& input_name);

MI_VARIANT
class PrimTranslator {
 public:
  struct TranslatedMaterial {
    mitsuba::ref<mitsuba::Object> bsdf = nullptr;
    std::optional<mitsuba::Properties> shape_emitter_props = std::nullopt;
    mitsuba::ref<mitsuba::Object> displacement_texture = nullptr;
  };

  static TranslatedMaterial BuildMaterial(const MaterialSpec& spec,
                                          const TextureCache& texture_cache);
  // Applies a parameter-values-only material edit to the existing Mitsuba
  // BSDF via its traversal interface (building a translated twin as the
  // value source). Returns false when the update cannot be applied
  // faithfully; the caller must then rebuild the material.
  // Resolved write slots and notification order for targeted material
  // parameter updates. Valid while the material's object structure is
  // unchanged (the same lifetime the in-place update guards guarantee);
  // rebuilds must discard it.
  struct MaterialParamSlots {
    // Traversal-name suffix -> (destination, type).
    std::vector<std::tuple<std::string, void*, const std::type_info*>> slots;
    // Children-first notification order with full parameter-name lists.
    std::vector<std::pair<mitsuba::Object*, std::vector<std::string>>>
        notify_order;
  };

  // Walks the live material once and resolves everything ApplyParamValues
  // needs. Cache the result per material; invalidate on rebuild.
  static MaterialParamSlots ResolveMaterialParamSlots(mitsuba::Object* bsdf);

  // Writes individual parameter values onto the live material without
  // building a twin: each entry maps a Mitsuba traversal-name suffix (e.g.
  // "base_color.value") to its new value. All slots are resolved before any
  // write; returns false (writing nothing) if any slot is missing,
  // ambiguous, or of an unsupported type — the caller then falls back to a
  // full rebuild path. On success the object tree is notified
  // children-first with full parameter-name lists.
  static bool ApplyMaterialParamValues(
      const MaterialParamSlots& slots,
      const std::vector<std::pair<std::string, VtValue>>& changes);

  static bool UpdateMaterialInPlace(mitsuba::Object* bsdf,
                                    const MaterialSpec& spec,
                                    const TextureCache& texture_cache);
  static mitsuba::ref<mitsuba::Object> LoadTexture(
      const mitsuba::Properties& props);

  struct TranslatedLight {
    mitsuba::ref<mitsuba::Shape<Float, Spectrum>> shape = nullptr;
    mitsuba::ref<mitsuba::Emitter<Float, Spectrum>> emitter = nullptr;
  };

  static TranslatedLight BuildLight(const LightSpec& spec);

  static void UpdateLightInPlace(mitsuba::Object* light_obj,
                                 const LightSpec& spec);

  static mitsuba::ref<mitsuba::Sensor<Float, Spectrum>> BuildSensor(
      const CameraSpec& spec, bool is_interactive = false);

  static void UpdateSensorInPlace(mitsuba::Object* sensor_obj,
                                  const CameraSpec& spec);

  static mitsuba::ref<mitsuba::Shape<Float, Spectrum>> BuildMesh(
      const SdfPath& id, const VtIntArray& face_indices,
      const PrimvarMap& primvars, mitsuba::Object* bsdf,
      mitsuba::Object* emitter_ptr, mitsuba::Object* sensor_ptr);

  static void UpdateMeshInPlace(mitsuba::Object* mesh_obj,
                                const VtIntArray& face_indices,
                                const PrimvarMap& primvars);

  static mitsuba::ref<mitsuba::Shape<Float, Spectrum>> BuildCurves(
      const CurveSpec& spec, mitsuba::Object* bsdf);

 private:
  static mitsuba::Properties BuildLightProperties(const LightSpec& spec);

  static mitsuba::ref<mitsuba::BSDF<Float, Spectrum>> DefaultBsdf(
      std::string_view id_str);

  static TranslatedMaterial ParsePreviewSurface(
      const HdMaterialNetwork2& network2,
      const HdMaterialNode2& preview_surface_node,
      const TextureCache& texture_cache);
};

PXR_NAMESPACE_CLOSE_SCOPE
