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
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>

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

}  // namespace

HdMitsubaMaterial::HdMitsubaMaterial(const SdfPath& id) : HdMaterial(id) {}

void HdMitsubaMaterial::Sync(HdSceneDelegate* scene_delegate,
                             HdRenderParam* render_param,
                             HdDirtyBits* dirty_bits) {
  const SdfPath& id = GetId();
  TF_DEBUG(HDMITSUBA_SYNC).Msg("HdMitsubaMaterial::Sync: %s\n", id.GetText());

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

  // Syncs an empty network, which rebuilds the material as the fallback BSDF.
  // This matters when a previously valid network becomes empty (e.g. its
  // surface output is disconnected interactively): the stale Mitsuba material
  // must be replaced rather than kept.
  auto sync_empty_network = [&]() {
    MaterialSpec spec;
    spec.id = id;
    spec.needs_rebuild = true;
    scene_manager->SyncMaterial(std::move(spec));
    *dirty_bits = HdChangeTracker::Clean;
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
  spec.network2 = std::move(network);
  spec.needs_rebuild = true;
  scene_manager->SyncMaterial(std::move(spec));
  *dirty_bits = HdChangeTracker::Clean;
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
