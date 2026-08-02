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

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <drjit-core/jit.h>
#include <mitsuba/core/bitmap.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/logger.h>
#include <mitsuba/core/object.h>
#include <mitsuba/core/thread.h>
#include <mitsuba/core/util.h>
#include <map>

#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/delegate.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include "hdmitsuba/render_delegate.h"

#include "hdmitsuba/render_param.h"
#include "hdmitsuba/scene_manager.h"

PXR_NAMESPACE_OPEN_SCOPE

using mitsuba::Object;

template <typename Scene>
struct MitsubaStaticState {
  MitsubaStaticState() {
    mitsuba::Thread::static_initialization();
    mitsuba::Logger::static_initialization();
    mitsuba::Bitmap::static_initialization();

    jit_init(static_cast<uint32_t>(JitBackend::LLVM));

    mitsuba::FileResolver* fr = mitsuba::file_resolver();
    mitsuba::fs::path base_path = mitsuba::util::library_path().parent_path();
    if (!fr->contains(base_path)) {
      fr->append(base_path);
    }

    Scene::static_accel_initialization();
  }

  ~MitsubaStaticState() {
    Scene::static_accel_shutdown();
    mitsuba::Bitmap::static_shutdown();
    mitsuba::Logger::static_shutdown();
    mitsuba::Thread::static_shutdown();
    jit_shutdown();
  }
};

template <typename Scene>
std::tuple<std::unique_ptr<HdMitsubaRenderDelegate>,
           std::unique_ptr<HdRenderIndex>, std::unique_ptr<UsdImagingDelegate>,
           SceneManager*, HdMitsubaRenderParam*>
CreateRenderDelegateStateObjects() {
  auto render_delegate = std::make_unique<HdMitsubaRenderDelegate>();
  auto render_index = std::unique_ptr<HdRenderIndex>(
      pxr::HdRenderIndex::New(render_delegate.get(), {}));
  auto scene_delegate =
      std::make_unique<UsdImagingDelegate>(render_index.get(), SdfPath("/"));
  auto* render_param =
      static_cast<HdMitsubaRenderParam*>(render_delegate->GetRenderParam());
  auto* scene_manager = render_param->GetScene();
  return std::make_tuple(std::move(render_delegate), std::move(render_index),
                         std::move(scene_delegate), scene_manager,
                         render_param);
}

// Minimal scene delegate that stores values for the harness sync task.
class HdMitsubaTestTaskDelegate final : public HdSceneDelegate {
 public:
  HdMitsubaTestTaskDelegate(HdRenderIndex* parent_index, const SdfPath& id)
      : HdSceneDelegate(parent_index, id) {}

  void SetValue(const TfToken& key, VtValue value) {
    values_[key] = std::move(value);
  }

  VtValue Get(const SdfPath& /*id*/, const TfToken& key) override {
    auto it = values_.find(key);
    return it == values_.end() ? VtValue() : it->second;
  }

 private:
  std::map<TfToken, VtValue, TfTokenFastArbitraryLessThan> values_;
};

// Task that enqueues the harness collection for sync and declares the
// geometry render tag; this mirrors the role HdxRenderTask plays in
// applications (rprims are only synced for render tags declared by tasks).
class HdMitsubaTestSyncTask final : public HdTask {
 public:
  HdMitsubaTestSyncTask(HdSceneDelegate* /*delegate*/, const SdfPath& id)
      : HdTask(id) {}

  void Sync(HdSceneDelegate* delegate, HdTaskContext* /*ctx*/,
            HdDirtyBits* dirty_bits) override {
    VtValue collection = delegate->Get(GetId(), HdTokens->collection);
    if (collection.IsHolding<HdRprimCollection>()) {
      delegate->GetRenderIndex().EnqueueCollectionToSync(
          collection.UncheckedGet<HdRprimCollection>());
    }
    *dirty_bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext* /*ctx*/, HdRenderIndex* /*index*/) override {}
  void Execute(HdTaskContext* /*ctx*/) override {}

  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector* kRenderTags =
        new TfTokenVector{HdRenderTagTokens->geometry};
    return *kRenderTags;
  }
};

// Hydra 2.0 test harness: the stage is imaged through the
// UsdImagingStageSceneIndex chain (UsdImagingCreateSceneIndices), inserted
// into the render index exactly like applications such as usdview do in scene
// index mode. Prims are created and synced by the render index's scene index
// emulation, and the resulting Mitsuba scene state can be inspected through
// the returned SceneManager.
struct SceneIndexTestHarness {
  // Destruction order (reverse of declaration): the scene index chain goes
  // away before the render index, which goes away before the render delegate.
  std::unique_ptr<HdMitsubaRenderDelegate> render_delegate;
  std::unique_ptr<HdRenderIndex> render_index;
  std::unique_ptr<HdMitsubaTestTaskDelegate> task_delegate;
  UsdImagingSceneIndices scene_indices;
  SdfPath task_id;
  SceneManager* scene_manager = nullptr;
  HdMitsubaRenderParam* render_param = nullptr;

  // Propagates pending USD edits through the scene index chain and syncs all
  // dirty prims in the render index (the equivalent of one Hydra sync round).
  void Sync() {
    scene_indices.stageSceneIndex->ApplyPendingUpdates();
    // Re-sync the task each round so that it re-enqueues the collection.
    render_index->GetChangeTracker().MarkTaskDirty(
        task_id, HdChangeTracker::DirtyCollection);
    HdTaskSharedPtrVector tasks = {render_index->GetTask(task_id)};
    HdTaskContext task_context;
    render_index->SyncAll(&tasks, &task_context);
  }
};

inline SceneIndexTestHarness CreateSceneIndexTestHarness(
    const UsdStageRefPtr& stage) {
  SceneIndexTestHarness harness;
  harness.render_delegate = std::make_unique<HdMitsubaRenderDelegate>();
  harness.render_index = std::unique_ptr<HdRenderIndex>(
      pxr::HdRenderIndex::New(harness.render_delegate.get(), {}));

  UsdImagingCreateSceneIndicesInfo create_info;
  create_info.stage = stage;
  harness.scene_indices = UsdImagingCreateSceneIndices(create_info);
  harness.scene_indices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  harness.render_index->InsertSceneIndex(
      harness.scene_indices.finalSceneIndex, SdfPath::AbsoluteRootPath());

  HdRprimCollection collection(HdTokens->geometry,
                               HdReprSelector(HdReprTokens->hull));
  collection.SetRootPath(SdfPath::AbsoluteRootPath());

  harness.task_delegate = std::make_unique<HdMitsubaTestTaskDelegate>(
      harness.render_index.get(), SdfPath("/hdMitsubaTestTask"));
  harness.task_id = SdfPath("/hdMitsubaTestTask/sync_task");
  harness.task_delegate->SetValue(HdTokens->collection, VtValue(collection));
  harness.render_index->InsertTask<HdMitsubaTestSyncTask>(
      harness.task_delegate.get(), harness.task_id);

  harness.render_param = static_cast<HdMitsubaRenderParam*>(
      harness.render_delegate->GetRenderParam());
  harness.scene_manager = harness.render_param->GetScene();

  harness.Sync();
  return harness;
}

PXR_NAMESPACE_CLOSE_SCOPE
