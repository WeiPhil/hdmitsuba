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

#ifndef HDMITSUBA_SCENE_INDEX_BACKEND_H_
#define HDMITSUBA_SCENE_INDEX_BACKEND_H_

// Native (Hydra 2.0) scene index consumption. The render index hands the
// delegate its terminal scene index (HdRenderDelegate::SetTerminalSceneIndex)
// and calls HdRenderDelegate::Update() at the start of every SyncAll; this
// backend observes the terminal index directly and translates change
// notifications into hdMitsuba specs, bypassing the emulated per-prim Sync
// machinery for the prim types it claims. Claimed prims' Sync methods become
// no-ops; unclaimed types (and hosts on Hydra versions without the 2.0
// protocol) continue through the prim path — the delegate is dual-mode with
// a single mechanism.

#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/synchronization/mutex.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include "hdmitsuba/prim_translation.h"

PXR_NAMESPACE_OPEN_SCOPE

class SceneManager;

class HdMitsubaSceneIndexBackend {
 public:
  HdMitsubaSceneIndexBackend();
  ~HdMitsubaSceneIndexBackend();

  // Registers the observer on the terminal scene index. No-op when native
  // consumption is disabled via HDMITSUBA_DISABLE_NATIVE_SCENE_INDEX.
  void Attach(const HdSceneIndexBaseRefPtr& terminal_scene_index);

  // Whether the native backend has claimed this prim (its Hydra prim Sync
  // must then be a no-op).
  bool Claimed(const SdfPath& id) const;

  // Drains queued notifications and translates claimed prims into specs.
  // Called from HdRenderDelegate::Update() (start of SyncAll).
  void ProcessUpdates(SceneManager* scene_manager);

 private:
  class Observer final : public HdSceneIndexObserver {
   public:
    explicit Observer(HdMitsubaSceneIndexBackend* backend)
        : backend_(backend) {}
    void PrimsAdded(const HdSceneIndexBase& sender,
                    const AddedPrimEntries& entries) override;
    void PrimsRemoved(const HdSceneIndexBase& sender,
                      const RemovedPrimEntries& entries) override;
    void PrimsDirtied(const HdSceneIndexBase& sender,
                      const DirtiedPrimEntries& entries) override;
    void PrimsRenamed(const HdSceneIndexBase& sender,
                      const RenamedPrimEntries& entries) override;

   private:
    HdMitsubaSceneIndexBackend* backend_;
  };

  static bool IsNativeType(const TfToken& prim_type);

  void TranslatePrim(const SdfPath& id, const TfToken& prim_type,
                     SceneManager* scene_manager);

  // Locator-gated fast path: for a value-only edit on a single material
  // node, patches the changed parameters straight onto the live BSDF and
  // the cached network, skipping full translation and the twin build.
  // Returns false when anything about the edit is not provably value-only.
  bool TryFastMaterialUpdate(const SdfPath& id,
                             const HdDataSourceLocatorSet& locators,
                             SceneManager* scene_manager);

  HdSceneIndexBaseRefPtr terminal_scene_index_;
  std::unique_ptr<Observer> observer_;

  mutable absl::Mutex queue_mutex_;
  std::vector<std::pair<SdfPath, TfToken>> pending_added_;
  std::vector<std::pair<SdfPath, HdDataSourceLocatorSet>> pending_dirtied_;
  std::vector<SdfPath> pending_removed_;

  mutable absl::Mutex claimed_mutex_;
  absl::flat_hash_set<SdfPath, SdfPath::Hash> claimed_;

  // Prim types as reported by PrimsAdded, for dispatching dirtied/removed
  // entries (which carry no type).
  absl::flat_hash_map<SdfPath, TfToken, SdfPath::Hash> prim_types_;

  // Per-prim translation state (previously prim-object members).
  absl::flat_hash_map<SdfPath, MaterialTranslationState, SdfPath::Hash>
      material_states_;
  absl::flat_hash_map<SdfPath, LightTranslationState, SdfPath::Hash>
      light_states_;
  absl::flat_hash_map<SdfPath, CameraTranslationState, SdfPath::Hash>
      camera_states_;
  absl::flat_hash_map<SdfPath, MeshTranslationState, SdfPath::Hash>
      mesh_states_;
  absl::flat_hash_map<SdfPath, CurvesTranslationState, SdfPath::Hash>
      curves_states_;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // HDMITSUBA_SCENE_INDEX_BACKEND_H_
