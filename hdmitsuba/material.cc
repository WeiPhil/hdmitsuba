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

#include "hdmitsuba/material.h"

#include "hdmitsuba/prim_translation.h"
#include "hdmitsuba/render_delegate.h"

#include <utility>

#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/assetPath.h>

#include "hdmitsuba/debug_codes.h"
#include "hdmitsuba/render_param.h"
#include "hdmitsuba/scene_manager.h"
#include "hdmitsuba/spec_types.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Temporarily convert from material network schema to material network2.
// In a later step, we will modify the prim_translator to directly work
// using the schema.
HdMaterialNetwork2 ConvertMaterialNetwork(
    const HdMaterialNetworkSchema& network_schema) {
  HdMaterialNetwork2 network;

  HdMaterialNodeContainerSchema nodes_schema = network_schema.GetNodes();
  if (!nodes_schema.IsDefined()) {
    return network;
  }

  for (const TfToken& node_name : nodes_schema.GetNames()) {
    HdMaterialNodeSchema node_schema = nodes_schema.Get(node_name);
    if (!node_schema.IsDefined()) continue;
    HdMaterialNode2 node;
    node.nodeTypeId = GetParam<TfToken>(
        node_schema.GetContainer(), HdMaterialNodeSchemaTokens->nodeIdentifier);

    // Parameters
    if (auto params_schema = node_schema.GetParameters()) {
      for (const TfToken& param_name : params_schema.GetNames()) {
        if (auto param_schema = params_schema.Get(param_name)) {
          if (auto val_ds = param_schema.GetValue()) {
            node.parameters[param_name] = val_ds->GetValue(0.0f);
          }
          // The Hydra 2.0 network schema carries the resolved color space
          // per parameter (UsdImaging folds a UsdUVTexture's
          // sourceColorSpace input into the file parameter's colorSpace
          // field) — structured data the flat Hydra 1.0 HdMaterialNetwork
          // parameter list had no place for. Encode it under the upstream
          // "colorSpace:<param>" convention for the translator.
          if (auto color_space_ds = param_schema.GetColorSpace()) {
            const TfToken color_space = color_space_ds->GetTypedValue(0.0f);
            if (!color_space.IsEmpty()) {
              node.parameters[TfToken("colorSpace:" +
                                      param_name.GetString())] =
                  VtValue(color_space);
            }
          }
        }
      }
    }

    // Input Connections
    if (auto connections_schema = node_schema.GetInputConnections()) {
      for (const TfToken& input_name : connections_schema.GetNames()) {
        if (auto vector_schema = connections_schema.Get(input_name)) {
          std::vector<HdMaterialConnection2> connections;
          for (size_t i = 0; i < vector_schema.GetNumElements(); ++i) {
            HdMaterialConnectionSchema conn_schema =
                vector_schema.GetElement(i);
            if (conn_schema.IsDefined()) {
              HdMaterialConnection2 conn;
              conn.upstreamNode = SdfPath(GetParam<TfToken>(
                  conn_schema.GetContainer(),
                  HdMaterialConnectionSchemaTokens->upstreamNodePath));
              conn.upstreamOutputName = GetParam<TfToken>(
                  conn_schema.GetContainer(),
                  HdMaterialConnectionSchemaTokens->upstreamNodeOutputName);
              connections.push_back(conn);
            }
          }
          node.inputConnections[input_name] = connections;
        }
      }
    }
    network.nodes[SdfPath(node_name)] = node;
  }

  auto terminals_schema = network_schema.GetTerminals();
  if (!terminals_schema) {
    return network;
  }

  for (const TfToken& terminal_name : terminals_schema.GetNames()) {
    if (auto conn_schema = terminals_schema.Get(terminal_name)) {
      HdMaterialConnection2 conn;
      conn.upstreamNode = SdfPath(GetParam<TfToken>(
          conn_schema.GetContainer(),
          HdMaterialConnectionSchemaTokens->upstreamNodePath));
      conn.upstreamOutputName = GetParam<TfToken>(
          conn_schema.GetContainer(),
          HdMaterialConnectionSchemaTokens->upstreamNodeOutputName);
      network.terminals[terminal_name] = conn;
    }
  }

  return network;
}

// Whether the value selects a plugin or a resource (file paths, enum-like
// switches): changing it changes the translated Mitsuba object structure.
bool IsStructuralValue(const VtValue& value) {
  return value.IsHolding<std::string>() || value.IsHolding<TfToken>() ||
         value.IsHolding<SdfAssetPath>();
}

bool IsSpecializationBoundary(float v) { return v == 0.0f || v == 1.0f; }

// Whether a value change can toggle construction-time specialization in the
// translated Mitsuba plugins. Mitsuba plugins commonly bake feature gates
// when they are built (e.g. the principled BSDF's has_metallic /
// has_clearcoat / opacity handling, driven by parameters being 0, 1 or an
// all-zero color); such gates are not visible through the traversal
// interface, so an in-place value copy would leave them stale. Changes that
// touch the 0/1 boundaries (or flip bools/ints, or cross an all-zero color)
// therefore count as structural and rebuild the material.
bool IsSpecializingValueChange(const VtValue& prev, const VtValue& next) {
  if (prev.IsHolding<bool>() || next.IsHolding<bool>() ||
      prev.IsHolding<int>() || next.IsHolding<int>()) {
    return prev != next;
  }
  if (prev.IsHolding<float>() && next.IsHolding<float>()) {
    const float a = prev.Get<float>();
    const float b = next.Get<float>();
    return a != b && (IsSpecializationBoundary(a) ||
                      IsSpecializationBoundary(b));
  }
  if (prev.IsHolding<GfVec3f>() && next.IsHolding<GfVec3f>()) {
    const GfVec3f a = prev.Get<GfVec3f>();
    const GfVec3f b = next.Get<GfVec3f>();
    return (a == GfVec3f(0.0f)) != (b == GfVec3f(0.0f));
  }
  if (prev.IsHolding<GfVec4f>() && next.IsHolding<GfVec4f>()) {
    const GfVec4f a = prev.Get<GfVec4f>();
    const GfVec4f b = next.Get<GfVec4f>();
    return (a == GfVec4f(0.0f)) != (b == GfVec4f(0.0f));
  }
  return false;
}

// True when `next` differs from `prev` at most in non-structural parameter
// *values*: identical node set, node types, connections, terminals and
// parameter keys, with string/token/asset parameters unchanged. Such edits
// can be applied to the existing Mitsuba BSDF in place.
bool OnlyParameterValuesChanged(const HdMaterialNetwork2& prev,
                                const HdMaterialNetwork2& next) {
  if (prev.terminals != next.terminals) {
    return false;
  }
  if (prev.nodes.size() != next.nodes.size()) {
    return false;
  }
  for (const auto& [path, next_node] : next.nodes) {
    auto prev_it = prev.nodes.find(path);
    if (prev_it == prev.nodes.end()) {
      return false;
    }
    const HdMaterialNode2& prev_node = prev_it->second;
    if (prev_node.nodeTypeId != next_node.nodeTypeId ||
        prev_node.inputConnections != next_node.inputConnections ||
        prev_node.parameters.size() != next_node.parameters.size()) {
      return false;
    }
    for (const auto& [name, next_value] : next_node.parameters) {
      auto prev_param_it = prev_node.parameters.find(name);
      if (prev_param_it == prev_node.parameters.end()) {
        return false;
      }
      const VtValue& prev_value = prev_param_it->second;
      if (IsStructuralValue(prev_value) || IsStructuralValue(next_value)) {
        if (prev_value != next_value) {
          return false;
        }
      } else if (IsSpecializingValueChange(prev_value, next_value)) {
        return false;
      }
    }
  }
  return true;
}

// In-place updates are limited to plain surface materials: displacement
// affects mesh geometry and emissive terminals turn into shape emitters,
// both of which are structural on the Mitsuba side.
bool IsSurfaceOnly(const HdMaterialNetwork2& network) {
  return network.terminals.size() == 1 &&
         network.terminals.count(HdMaterialTerminalTokens->surface) == 1;
}

}  // namespace

HdMitsubaMaterial::HdMitsubaMaterial(const SdfPath& id) : HdMaterial(id) {}

void HdMitsubaMaterial::Sync(HdSceneDelegate* scene_delegate,
                             HdRenderParam* render_param,
                             HdDirtyBits* dirty_bits) {
  const SdfPath& id = GetId();
  TF_DEBUG(HDMITSUBA_SYNC).Msg("HdMitsubaMaterial::Sync: %s\n", id.GetText());

  if (static_cast<HdMitsubaRenderDelegate*>(
          scene_delegate->GetRenderIndex().GetRenderDelegate())
          ->NativeClaimed(id)) {
    *dirty_bits = HdChangeTracker::Clean;
    return;
  }

  if (!(*dirty_bits & DirtyBits::DirtyParams) &&
      !(*dirty_bits & DirtyBits::DirtyResource)) {
    *dirty_bits = HdChangeTracker::Clean;
    return;
  }

  HdSceneIndexBaseRefPtr scene_index =
      scene_delegate->GetRenderIndex().GetTerminalSceneIndex();
  if (!TF_VERIFY(scene_index)) {
    return;
  }
  SceneManager* scene_manager =
      static_cast<HdMitsubaRenderParam*>(render_param)->GetScene();
  TranslateMaterialPrim(scene_index, id, &translation_state_, scene_manager);
  *dirty_bits = HdChangeTracker::Clean;
}


bool MaterialParamChangeIsStructural(const VtValue& prev,
                                     const VtValue& next) {
  if (prev.GetTypeid() != next.GetTypeid()) {
    return true;
  }
  if (IsStructuralValue(prev) || IsStructuralValue(next)) {
    return prev != next;
  }
  return IsSpecializingValueChange(prev, next);
}

void TranslateMaterialPrim(const HdSceneIndexBaseRefPtr& scene_index,
                           const SdfPath& id, MaterialTranslationState* state,
                           SceneManager* scene_manager) {
  // Syncs an empty network, which rebuilds the material as the fallback BSDF.
  // This matters when a previously valid network becomes empty (e.g. its
  // surface output is disconnected interactively): the stale Mitsuba material
  // must be replaced rather than kept.
  auto sync_empty_network = [&]() {
    state->has_last_network = false;
    state->last_network = HdMaterialNetwork2();
    MaterialSpec spec;
    spec.id = id;
    spec.needs_rebuild = true;
    scene_manager->SyncMaterial(std::move(spec));
  };

  HdMaterialSchema materialSchema =
      HdMaterialSchema::GetFromParent(scene_index->GetPrim(id).dataSource);
  if (!materialSchema.IsDefined()) {
    sync_empty_network();
    return;
  }
  // Hydra 2.0: with the stage scene index front-end, the material data source
  // holds one network per render context (e.g. "" universal from
  // outputs:surface / outputs:displacement, "mitsuba" from
  // outputs:mitsuba:surface / outputs:mitsuba:displacement). A material may
  // mix contexts per terminal (e.g. a universal preview surface with a
  // mitsuba-specific displacement), so resolution must happen per terminal:
  // start from the universal network and overlay the "mitsuba" context's
  // nodes and terminals on top. Behind the legacy pipeline the emulated data
  // source only has the universal network (already resolved for our context
  // by UsdImaging), so the overlay is a no-op there.
  static const TfToken kMitsubaRenderContext("mitsuba");
  HdMaterialNetworkSchema universal_schema =
      materialSchema.GetMaterialNetwork();
  HdMaterialNetworkSchema mitsuba_schema =
      materialSchema.GetMaterialNetwork(kMitsubaRenderContext);
  if (!universal_schema.IsDefined() && !mitsuba_schema.IsDefined()) {
    sync_empty_network();
    return;
  }

  HdMaterialNetwork2 network;
  if (universal_schema.IsDefined()) {
    network = ConvertMaterialNetwork(universal_schema);
  }
  if (mitsuba_schema.IsDefined()) {
    HdMaterialNetwork2 mitsuba_network =
        ConvertMaterialNetwork(mitsuba_schema);
    for (auto& [path, node] : mitsuba_network.nodes) {
      network.nodes[path] = std::move(node);
    }
    for (const auto& [terminal, connection] : mitsuba_network.terminals) {
      network.terminals[terminal] = connection;
    }
  }

  MaterialSpec spec;
  spec.id = id;
  // Edits that only change (non-structural) parameter values update the
  // existing Mitsuba BSDF in place; everything else rebuilds it.
  const bool update_in_place = state->has_last_network && IsSurfaceOnly(network) &&
                               IsSurfaceOnly(state->last_network) &&
                               OnlyParameterValuesChanged(state->last_network,
                                                          network);
  if (update_in_place) {
    TF_DEBUG(HDMITSUBA_SYNC)
        .Msg("HdMitsubaMaterial::Sync %s: parameter-values-only edit, "
             "updating the Mitsuba BSDF in place\n",
             id.GetText());
    spec.needs_rebuild = false;
    spec.dirty_bits = HdMaterial::DirtyParams;
  } else {
    spec.needs_rebuild = true;
  }
  state->last_network = network;
  state->has_last_network = true;
  spec.network2 = std::move(network);
  scene_manager->SyncMaterial(std::move(spec));
}

HdDirtyBits HdMitsubaMaterial::GetInitialDirtyBitsMask() const {
  return DirtyBits::DirtyResource | DirtyBits::DirtyParams;
}

void HdMitsubaMaterial::Finalize(HdRenderParam* renderParam) {
  auto* mitsuba_render_param = static_cast<HdMitsubaRenderParam*>(renderParam);
  mitsuba_render_param->GetScene()->RemoveMaterial(GetId());
  HdMaterial::Finalize(renderParam);
}

PXR_NAMESPACE_CLOSE_SCOPE
