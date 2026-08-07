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

#include "hdmitsuba/scene_index_backend.h"

#include <memory>
#include <utility>

#include <pxr/base/tf/stopwatch.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/pxr.h>

#include "hdmitsuba/config.h"
#include "hdmitsuba/debug_codes.h"
#include "hdmitsuba/scene_manager.h"

PXR_NAMESPACE_OPEN_SCOPE

HdMitsubaSceneIndexBackend::HdMitsubaSceneIndexBackend() = default;

HdMitsubaSceneIndexBackend::~HdMitsubaSceneIndexBackend() {
  if (terminal_scene_index_ && observer_) {
    terminal_scene_index_->RemoveObserver(HdSceneIndexObserverPtr(
        observer_.get()));
  }
}

void HdMitsubaSceneIndexBackend::Attach(
    const HdSceneIndexBaseRefPtr& terminal_scene_index) {
  if (HdMitsubaConfig::GetInstance().disable_native_scene_index > 0) {
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native scene index backend disabled via "
             "HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX\n");
    return;
  }
  if (!terminal_scene_index) {
    return;
  }
  terminal_scene_index_ = terminal_scene_index;
  observer_ = std::make_unique<Observer>(this);
  terminal_scene_index_->AddObserver(
      HdSceneIndexObserverPtr(observer_.get()));
  TF_DEBUG(HDMITSUBA_NATIVE)
      .Msg("Native scene index backend attached to the terminal scene "
           "index\n");
}

bool HdMitsubaSceneIndexBackend::IsNativeType(const TfToken& prim_type) {
  return prim_type == HdPrimTypeTokens->material ||
         prim_type == HdPrimTypeTokens->camera ||
         prim_type == HdPrimTypeTokens->sphereLight ||
         prim_type == HdPrimTypeTokens->domeLight ||
         prim_type == HdPrimTypeTokens->distantLight ||
         prim_type == HdPrimTypeTokens->rectLight ||
         prim_type == HdPrimTypeTokens->diskLight;
}

bool HdMitsubaSceneIndexBackend::Claimed(const SdfPath& id) const {
  absl::MutexLock lock(&claimed_mutex_);
  return claimed_.contains(id);
}

void HdMitsubaSceneIndexBackend::Observer::PrimsAdded(
    const HdSceneIndexBase& /*sender*/, const AddedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const AddedPrimEntry& entry : entries) {
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("  observer added: %s (%s)\n", entry.primPath.GetText(),
             entry.primType.GetText());
    backend_->pending_added_.emplace_back(entry.primPath, entry.primType);
  }
}

void HdMitsubaSceneIndexBackend::Observer::PrimsRemoved(
    const HdSceneIndexBase& /*sender*/, const RemovedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const RemovedPrimEntry& entry : entries) {
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("  observer removed: %s\n", entry.primPath.GetText());
    backend_->pending_removed_.push_back(entry.primPath);
  }
}

void HdMitsubaSceneIndexBackend::Observer::PrimsDirtied(
    const HdSceneIndexBase& /*sender*/, const DirtiedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const DirtiedPrimEntry& entry : entries) {
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("  observer dirtied: %s\n", entry.primPath.GetText());
    backend_->pending_dirtied_.emplace_back(entry.primPath,
                                            entry.dirtyLocators);
  }
}

void HdMitsubaSceneIndexBackend::Observer::PrimsRenamed(
    const HdSceneIndexBase& sender, const RenamedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const RenamedPrimEntry& entry : entries) {
    backend_->pending_removed_.push_back(entry.oldPrimPath);
    backend_->pending_added_.emplace_back(
        entry.newPrimPath, sender.GetPrim(entry.newPrimPath).primType);
  }
}

void HdMitsubaSceneIndexBackend::TranslatePrim(const SdfPath& id,
                                               const TfToken& prim_type,
                                               SceneManager* scene_manager) {
  if (prim_type == HdPrimTypeTokens->material) {
    TranslateMaterialPrim(terminal_scene_index_, id, &material_states_[id],
                          scene_manager);
  } else if (prim_type == HdPrimTypeTokens->camera) {
    TranslateCameraPrim(terminal_scene_index_, id, HdCamera::AllDirty,
                        &camera_states_[id], scene_manager);
  } else if (prim_type == HdPrimTypeTokens->mesh) {
    TranslateMeshPrim(terminal_scene_index_, id, HdChangeTracker::AllDirty,
                      &mesh_states_[id], scene_manager);
  } else if (prim_type == HdPrimTypeTokens->basisCurves) {
    TranslateCurvesPrim(terminal_scene_index_, id, HdChangeTracker::AllDirty,
                        &curves_states_[id], scene_manager);
  } else {
    TranslateLightPrim(terminal_scene_index_, id, prim_type, HdLight::AllDirty,
                       &light_states_[id], scene_manager);
  }
}


bool HdMitsubaSceneIndexBackend::TryFastMaterialUpdate(
    const SdfPath& id, const HdDataSourceLocatorSet& locators,
    SceneManager* scene_manager) {
  auto state_it = material_states_.find(id);
  if (state_it == material_states_.end() ||
      !state_it->second.has_last_network) {
    return false;
  }
  HdMaterialNetwork2& cached = state_it->second.last_network;

  // 1. Gate on the locators: every one must stay inside
  //    material/<context>/{nodes/<node>, terminals/...}, and collect the
  //    dirtied node names.
  static const TfToken kNodes("nodes");
  static const TfToken kTerminals("terminals");
  absl::flat_hash_set<TfToken, TfToken::HashFunctor> dirty_nodes;
  for (const HdDataSourceLocator& locator : locators) {
    if (locator.GetElementCount() < 3 ||
        locator.GetElement(0) != HdMaterialSchema::GetSchemaToken()) {
      return false;
    }
    const TfToken& section = locator.GetElement(2);
    if (section == kNodes) {
      if (locator.GetElementCount() < 4) {
        return false;  // Whole-nodes-container invalidation: not value-only.
      }
      dirty_nodes.insert(locator.GetElement(3));
    } else if (section != kTerminals) {
      return false;
    }
  }
  if (dirty_nodes.empty()) {
    return false;
  }

  HdContainerDataSourceHandle prim_source =
      terminal_scene_index_->GetPrim(id).dataSource;
  if (!prim_source) {
    return false;
  }
  HdMaterialSchema material_schema =
      HdMaterialSchema::GetFromParent(prim_source);
  if (!material_schema.IsDefined()) {
    return false;
  }
  // A render-context-specific network would require overlay resolution;
  // take the full path for those (rare) materials.
  static const TfToken kMitsubaRenderContext("mitsuba");
  if (material_schema.GetMaterialNetwork(kMitsubaRenderContext).IsDefined()) {
    return false;
  }
  HdMaterialNetworkSchema network = material_schema.GetMaterialNetwork();
  if (!network.IsDefined()) {
    return false;
  }

  // 2. Terminals must be unchanged (a rewire is structural).
  HdContainerDataSourceHandle terminals_container =
      network.GetTerminals().GetContainer();
  size_t terminal_count = 0;
  if (terminals_container) {
    for (const TfToken& name : terminals_container->GetNames()) {
      ++terminal_count;
      HdMaterialConnectionSchema connection(HdContainerDataSource::Cast(
          terminals_container->Get(name)));
      if (!connection.IsDefined()) {
        return false;
      }
      auto cached_terminal = cached.terminals.find(name);
      if (cached_terminal == cached.terminals.end()) {
        return false;
      }
      HdTokenDataSourceHandle node_token = connection.GetUpstreamNodePath();
      if (!node_token ||
          cached_terminal->second.upstreamNode.GetNameToken() !=
              node_token->GetTypedValue(0.0f)) {
        return false;
      }
    }
  }
  if (terminal_count != cached.terminals.size()) {
    return false;
  }

  // 3. Per dirtied node: diff current parameter values against the cached
  //    network. Only free (unconnected) value parameters may change, and no
  //    change may be structural.
  HdContainerDataSourceHandle nodes_container = network.GetNodes().GetContainer();
  if (!nodes_container) {
    return false;
  }
  struct NodeChanges {
    SdfPath node_path;
    std::vector<std::pair<TfToken, VtValue>> changes;
  };
  std::vector<NodeChanges> per_node;
  for (const TfToken& node_name : dirty_nodes) {
    // Resolve the cached node whose path ends in this node name; require
    // uniqueness.
    SdfPath node_path;
    HdMaterialNode2* cached_node = nullptr;
    for (auto& [path, node] : cached.nodes) {
      if (path.GetNameToken() == node_name) {
        if (cached_node != nullptr) {
          return false;
        }
        node_path = path;
        cached_node = &node;
      }
    }
    if (cached_node == nullptr) {
      return false;
    }
    HdMaterialNodeSchema node_schema(HdContainerDataSource::Cast(
        nodes_container->Get(node_name)));
    if (!node_schema.IsDefined()) {
      return false;
    }
    // The node's identity and connection set must be unchanged: a
    // (dis)connect can leave every parameter value identical (an input with
    // an authored fallback value) yet still change the translated material
    // structurally.
    if (HdTokenDataSourceHandle identifier = node_schema.GetNodeIdentifier()) {
      if (identifier->GetTypedValue(0.0f) != cached_node->nodeTypeId) {
        return false;
      }
    } else {
      return false;
    }
    {
      HdContainerDataSourceHandle connections_container =
          node_schema.GetInputConnections().GetContainer();
      size_t connection_count = 0;
      if (connections_container) {
        for (const TfToken& input_name : connections_container->GetNames()) {
          ++connection_count;
          auto cached_connection =
              cached_node->inputConnections.find(input_name);
          if (cached_connection == cached_node->inputConnections.end()) {
            return false;  // New connection: structural.
          }
          HdMaterialConnectionVectorSchema vector_schema(
              HdVectorDataSource::Cast(connections_container->Get(input_name)));
          if (vector_schema.GetNumElements() !=
              cached_connection->second.size()) {
            return false;
          }
          for (size_t i = 0; i < vector_schema.GetNumElements(); ++i) {
            HdMaterialConnectionSchema connection = vector_schema.GetElement(i);
            if (!connection.IsDefined()) {
              return false;
            }
            HdTokenDataSourceHandle upstream = connection.GetUpstreamNodePath();
            if (!upstream ||
                cached_connection->second[i].upstreamNode.GetNameToken() !=
                    upstream->GetTypedValue(0.0f)) {
              return false;
            }
          }
        }
      }
      if (connection_count != cached_node->inputConnections.size()) {
        return false;  // A connection was removed: structural.
      }
    }
    HdContainerDataSourceHandle params_container =
        node_schema.GetParameters().GetContainer();
    if (!params_container) {
      return false;
    }
    NodeChanges node_changes;
    node_changes.node_path = node_path;
    size_t param_count = 0;
    for (const TfToken& param_name : params_container->GetNames()) {
      ++param_count;
      HdMaterialNodeParameterSchema param_schema(HdContainerDataSource::Cast(
          params_container->Get(param_name)));
      if (!param_schema.IsDefined()) {
        return false;
      }
      HdSampledDataSourceHandle value_source = param_schema.GetValue();
      if (!value_source) {
        return false;
      }
      VtValue value = value_source->GetValue(0.0f);
      // A changed resolved color space (e.g. a sourceColorSpace edit that
      // UsdImaging folds into the parameter's colorSpace field) alters how
      // the texture is loaded: structural, take the full path.
      {
        static const std::string kColorSpacePrefix = "colorSpace:";
        TfToken schema_space;
        if (HdTokenDataSourceHandle space = param_schema.GetColorSpace()) {
          schema_space = space->GetTypedValue(0.0f);
        }
        auto cached_space = cached_node->parameters.find(
            TfToken(kColorSpacePrefix + param_name.GetString()));
        const TfToken cached_space_token =
            cached_space != cached_node->parameters.end() &&
                    cached_space->second.IsHolding<TfToken>()
                ? cached_space->second.UncheckedGet<TfToken>()
                : TfToken();
        if (schema_space != cached_space_token) {
          return false;
        }
      }
      auto cached_param = cached_node->parameters.find(param_name);
      if (cached_param == cached_node->parameters.end()) {
        return false;  // New parameter: structural.
      }
      if (cached_param->second == value) {
        continue;
      }
      if (cached_node->inputConnections.count(param_name) > 0) {
        return false;  // Connected input: the edit is not a free value.
      }
      if (MaterialParamChangeIsStructural(cached_param->second, value)) {
        return false;
      }
      node_changes.changes.emplace_back(param_name, value);
    }
    // Count only value parameters in the cache (colorSpace bookkeeping
    // entries are our own convention, prefixed "colorSpace:").
    size_t cached_value_params = 0;
    for (const auto& [name, _] : cached_node->parameters) {
      if (name.GetString().rfind("colorSpace:", 0) != 0) {
        ++cached_value_params;
      }
    }
    if (param_count != cached_value_params) {
      return false;  // A parameter appeared or vanished: structural.
    }
    if (!node_changes.changes.empty()) {
      per_node.push_back(std::move(node_changes));
    }
  }
  if (per_node.empty()) {
    return true;  // Nothing actually changed (e.g. duplicate notification).
  }

  // 4. Write the values onto the live BSDF and patch the cached network.
  for (const NodeChanges& node_changes : per_node) {
    if (!scene_manager->UpdateMaterialValues(id, node_changes.node_path,
                                             node_changes.changes)) {
      return false;
    }
    auto& cached_params = cached.nodes[node_changes.node_path].parameters;
    for (const auto& [param, value] : node_changes.changes) {
      cached_params[param] = value;
    }
  }
  TF_DEBUG(HDMITSUBA_NATIVE)
      .Msg("Native fast update: %s (%zu nodes)\n", id.GetText(),
           per_node.size());
  return true;
}

void HdMitsubaSceneIndexBackend::ProcessUpdates(SceneManager* scene_manager) {
  if (!terminal_scene_index_ || !scene_manager) {
    return;
  }

  std::vector<std::pair<SdfPath, TfToken>> added;
  std::vector<std::pair<SdfPath, HdDataSourceLocatorSet>> dirtied;
  std::vector<SdfPath> removed;
  {
    absl::MutexLock lock(&queue_mutex_);
    added.swap(pending_added_);
    dirtied.swap(pending_dirtied_);
    removed.swap(pending_removed_);
  }
  if (added.empty() && dirtied.empty() && removed.empty()) {
    return;
  }

  for (const SdfPath& id : removed) {
    auto type_it = prim_types_.find(id);
    if (type_it == prim_types_.end()) {
      continue;
    }
    const TfToken prim_type = type_it->second;
    prim_types_.erase(type_it);
    {
      absl::MutexLock lock(&claimed_mutex_);
      claimed_.erase(id);
    }
    if (prim_type == HdPrimTypeTokens->material) {
      scene_manager->RemoveMaterial(id);
      material_states_.erase(id);
    } else if (prim_type == HdPrimTypeTokens->camera) {
      camera_states_.erase(id);
    } else if (prim_type == HdPrimTypeTokens->mesh ||
               prim_type == HdPrimTypeTokens->basisCurves) {
      scene_manager->RemoveShape(id);
      mesh_states_.erase(id);
      curves_states_.erase(id);
    } else {
      scene_manager->RemoveLight(id);
      light_states_.erase(id);
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native remove: %s (%s)\n", id.GetText(), prim_type.GetText());
  }

  // 1. Process materials first so bound materials are registered before meshes
  for (const auto& [id, prim_type] : added) {
    if (prim_type == HdPrimTypeTokens->material) {
      prim_types_[id] = prim_type;
      {
        absl::MutexLock lock(&claimed_mutex_);
        claimed_.insert(id);
      }
      TF_DEBUG(HDMITSUBA_NATIVE)
          .Msg("Native add: %s (%s)\n", id.GetText(), prim_type.GetText());
      TranslatePrim(id, prim_type, scene_manager);
    }
  }

  // 2. Process other prims
  for (const auto& [id, prim_type] : added) {
    if (prim_type != HdPrimTypeTokens->material && IsNativeType(prim_type)) {
      prim_types_[id] = prim_type;
      {
        absl::MutexLock lock(&claimed_mutex_);
        claimed_.insert(id);
      }
      TF_DEBUG(HDMITSUBA_NATIVE)
          .Msg("Native add: %s (%s)\n", id.GetText(), prim_type.GetText());
      TranslatePrim(id, prim_type, scene_manager);
    }
  }

  // A single edit fans out into several dirtied entries per prim (one per
  // locator batch), and a prim added this round is already translated:
  // deduplicate so each prim translates at most once per update.
  absl::flat_hash_set<SdfPath, SdfPath::Hash> translated_this_round;
  translated_this_round.reserve(added.size() + dirtied.size());
  for (const auto& [id, prim_type] : added) {
    if (IsNativeType(prim_type)) {
      translated_this_round.insert(id);
    }
  }
  // Merge each prim's locators across its dirty entries first.
  absl::flat_hash_map<SdfPath, HdDataSourceLocatorSet, SdfPath::Hash>
      dirty_locators;
  for (auto& [id, locators] : dirtied) {
    if (translated_this_round.contains(id)) {
      continue;
    }
    dirty_locators[id].insert(locators);
  }

  TfStopwatch translate_watch;
  translate_watch.Start();
  size_t translated_count = 0;
  size_t fast_count = 0;

  // 1. Process dirty materials first
  for (const auto& [id, locators] : dirty_locators) {
    auto type_it = prim_types_.find(id);
    if (type_it == prim_types_.end() || type_it->second != HdPrimTypeTokens->material) {
      continue;
    }
    if (TryFastMaterialUpdate(id, locators, scene_manager)) {
      ++fast_count;
      continue;
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native dirty: %s (%s)\n", id.GetText(),
             type_it->second.GetText());
    TranslatePrim(id, type_it->second, scene_manager);
    ++translated_count;
  }

  // 2. Process other dirty prims
  for (const auto& [id, locators] : dirty_locators) {
    auto type_it = prim_types_.find(id);
    if (type_it == prim_types_.end() || type_it->second == HdPrimTypeTokens->material) {
      continue;
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native dirty: %s (%s)\n", id.GetText(),
             type_it->second.GetText());
    TranslatePrim(id, type_it->second, scene_manager);
    ++translated_count;
  }

  translate_watch.Stop();
  if (translated_count + fast_count > 0) {
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native update round: %zu fast-patched, %zu translated in "
             "%.2f ms\n",
             fast_count, translated_count, translate_watch.GetSeconds() * 1e3);
  }
}

PXR_NAMESPACE_CLOSE_SCOPE
