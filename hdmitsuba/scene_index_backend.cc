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

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/material.h>
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
    backend_->pending_added_.emplace_back(entry.primPath, entry.primType);
  }
}

void HdMitsubaSceneIndexBackend::Observer::PrimsRemoved(
    const HdSceneIndexBase& /*sender*/, const RemovedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const RemovedPrimEntry& entry : entries) {
    backend_->pending_removed_.push_back(entry.primPath);
  }
}

void HdMitsubaSceneIndexBackend::Observer::PrimsDirtied(
    const HdSceneIndexBase& /*sender*/, const DirtiedPrimEntries& entries) {
  absl::MutexLock lock(&backend_->queue_mutex_);
  for (const DirtiedPrimEntry& entry : entries) {
    backend_->pending_dirtied_.push_back(entry.primPath);
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
  } else {
    TranslateLightPrim(terminal_scene_index_, id, prim_type, HdLight::AllDirty,
                       &light_states_[id], scene_manager);
  }
}

void HdMitsubaSceneIndexBackend::ProcessUpdates(SceneManager* scene_manager) {
  if (!terminal_scene_index_ || !scene_manager) {
    return;
  }

  std::vector<std::pair<SdfPath, TfToken>> added;
  std::vector<SdfPath> dirtied;
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
    } else {
      scene_manager->RemoveLight(id);
      light_states_.erase(id);
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native remove: %s (%s)\n", id.GetText(), prim_type.GetText());
  }

  for (const auto& [id, prim_type] : added) {
    if (!IsNativeType(prim_type)) {
      continue;
    }
    prim_types_[id] = prim_type;
    {
      absl::MutexLock lock(&claimed_mutex_);
      claimed_.insert(id);
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native add: %s (%s)\n", id.GetText(), prim_type.GetText());
    TranslatePrim(id, prim_type, scene_manager);
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
  for (const SdfPath& id : dirtied) {
    if (!translated_this_round.insert(id).second) {
      continue;
    }
    auto type_it = prim_types_.find(id);
    if (type_it == prim_types_.end()) {
      continue;
    }
    TF_DEBUG(HDMITSUBA_NATIVE)
        .Msg("Native dirty: %s (%s)\n", id.GetText(),
             type_it->second.GetText());
    TranslatePrim(id, type_it->second, scene_manager);
  }
}

PXR_NAMESPACE_CLOSE_SCOPE
