// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_TILES_TILE_MANAGER_H_
#define CC_TILES_TILE_MANAGER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/sequenced_task_runner.h"
#include "base/values.h"
#include "cc/base/unique_notifier.h"
#include "cc/raster/raster_buffer_provider.h"
#include "cc/raster/raster_source.h"
#include "cc/resources/memory_history.h"
#include "cc/resources/resource_pool.h"
#include "cc/tiles/checker_image_tracker.h"
#include "cc/tiles/decoded_image_tracker.h"
#include "cc/tiles/eviction_tile_priority_queue.h"
#include "cc/tiles/image_controller.h"
#include "cc/tiles/raster_tile_priority_queue.h"
#include "cc/tiles/tile.h"
#include "cc/tiles/tile_draw_info.h"
#include "cc/tiles/tile_manager_settings.h"
#include "cc/tiles/tile_task_manager.h"

namespace base {
namespace trace_event {
class ConvertableToTraceFormat;
class TracedValue;
}
}

namespace cc {
class ImageDecodeCache;

class CC_EXPORT TileManagerClient {
 public:
  // Called when all tiles marked as required for activation are ready to draw.
  virtual void NotifyReadyToActivate() = 0;

  // Called when all tiles marked as required for draw are ready to draw.
  virtual void NotifyReadyToDraw() = 0;

  // Called when all tile tasks started by the most recent call to PrepareTiles
  // are completed.
  virtual void NotifyAllTileTasksCompleted() = 0;

  // Called when the visible representation of a tile might have changed. Some
  // examples are:
  // - Tile version initialized.
  // - Tile resources freed.
  // - Tile marked for on-demand raster.
  virtual void NotifyTileStateChanged(const Tile* tile) = 0;

  // Given an empty raster tile priority queue, this will build a priority queue
  // that will return tiles in order in which they should be rasterized.
  // Note if the queue was previous built, Reset must be called on it.
  virtual std::unique_ptr<RasterTilePriorityQueue> BuildRasterQueue(
      TreePriority tree_priority,
      RasterTilePriorityQueue::Type type) = 0;

  // Given an empty eviction tile priority queue, this will build a priority
  // queue that will return tiles in order in which they should be evicted.
  // Note if the queue was previous built, Reset must be called on it.
  virtual std::unique_ptr<EvictionTilePriorityQueue> BuildEvictionQueue(
      TreePriority tree_priority) = 0;

  // Informs the client that due to the currently rasterizing (or scheduled to
  // be rasterized) tiles, we will be in a position that will likely require a
  // draw. This can be used to preemptively start a frame.
  virtual void SetIsLikelyToRequireADraw(bool is_likely_to_require_a_draw) = 0;

  // Requests the color space into which tiles should be rasterized.
  virtual gfx::ColorSpace GetRasterColorSpace() const = 0;

  // Requests that a pending tree be scheduled to invalidate content on the
  // pending on active tree. This is currently used when tiles that are
  // rasterized with missing images need to be invalidated.
  virtual void RequestImplSideInvalidation() = 0;

 protected:
  virtual ~TileManagerClient() {}
};

struct RasterTaskCompletionStats {
  RasterTaskCompletionStats();

  size_t completed_count;
  size_t canceled_count;
};
std::unique_ptr<base::trace_event::ConvertableToTraceFormat>
RasterTaskCompletionStatsAsValue(const RasterTaskCompletionStats& stats);

// This class manages tiles, deciding which should get rasterized and which
// should no longer have any memory assigned to them. Tile objects are "owned"
// by layers; they automatically register with the manager when they are
// created, and unregister from the manager when they are deleted.
//
// The TileManager coordinates scheduling of prioritized raster and decode work
// across 2 different subsystems, namely the TaskGraphRunner used primarily for
// raster work and images which must be decoded before rasterization of a tile
// can proceed, and the CheckerImageTracker used for images decoded
// asynchronously from raster using the |image_worker_task_runner|. The order in
// which work is scheduled across these systems is as follows:
//
// 1) RequiredForActivation/Draw Tiles: These are the highest priority tiles
// which block scheduling of any decode work for checkered-images.
//
// 2) Pre-paint Tiles: These are offscreen tiles which fall within the
// pre-raster distance. The work for these tiles continues in parallel with the
// decode work for checkered images from visible/pre-paint tiles.
//
// 3) Pre-decode Tiles: These are offscreen tiles which are outside the
// pre-raster distance but have their images pre-decoded and locked. Finishing
// work for these tiles on the TaskGraph blocks starting decode work for
// checker-imaged pre-decode tiles.

class CC_EXPORT TileManager : CheckerImageTrackerClient {
 public:
  TileManager(TileManagerClient* client,
              base::SequencedTaskRunner* origin_task_runner,
              scoped_refptr<base::SequencedTaskRunner> image_worker_task_runner,
              size_t scheduled_raster_task_limit,
              const TileManagerSettings& tile_manager_settings);
  ~TileManager() override;

  // Assigns tile memory and schedules work to prepare tiles for drawing.
  // - Runs client_->NotifyReadyToActivate() when all tiles required for
  // activation are prepared, or failed to prepare due to OOM.
  // - Runs client_->NotifyReadyToDraw() when all tiles required draw are
  // prepared, or failed to prepare due to OOM.
  bool PrepareTiles(const GlobalStateThatImpactsTilePriority& state);

  // Synchronously finish any in progress work, cancel the rest, and clean up as
  // much resources as possible. Also, prevents any future work until a
  // SetResources call.
  void FinishTasksAndCleanUp();

  // Set the new given resource pool and tile task runner. Note that
  // FinishTasksAndCleanUp must be called in between consecutive calls to
  // SetResources.
  void SetResources(ResourcePool* resource_pool,
                    ImageDecodeCache* image_decode_cache,
                    TaskGraphRunner* task_graph_runner,
                    RasterBufferProvider* raster_buffer_provider,
                    size_t scheduled_raster_task_limit,
                    bool use_gpu_rasterization);

  // This causes any completed raster work to finalize, so that tiles get up to
  // date draw information.
  void Flush();

  // Called when the required-for-activation/required-for-draw state of tiles
  // may have changed.
  void DidModifyTilePriorities();

  std::unique_ptr<Tile> CreateTile(const Tile::CreateInfo& info,
                                   int layer_id,
                                   int source_frame_number,
                                   int flags,
                                   bool can_use_lcd_text);

  bool IsReadyToActivate() const;
  bool IsReadyToDraw() const;

  const PaintImageIdFlatSet& TakeImagesToInvalidateOnSyncTree();
  void DidActivateSyncTree();
  void ClearCheckerImageTracking(bool can_clear_decode_policy_tracking);

  std::unique_ptr<base::trace_event::ConvertableToTraceFormat>
  BasicStateAsValue() const;
  void BasicStateAsValueInto(base::trace_event::TracedValue* dict) const;
  const MemoryHistory::Entry& memory_stats_from_last_assign() const {
    return memory_stats_from_last_assign_;
  }

  // Public methods for testing.
  void InitializeTilesWithResourcesForTesting(const std::vector<Tile*>& tiles) {
    for (size_t i = 0; i < tiles.size(); ++i) {
      TileDrawInfo& draw_info = tiles[i]->draw_info();
      draw_info.set_resource(
          resource_pool_->AcquireResource(
              tiles[i]->desired_texture_size(),
              raster_buffer_provider_->GetResourceFormat(false),
              client_->GetRasterColorSpace()),
          false);
      draw_info.set_resource_ready_for_draw();
    }
  }

  void ReleaseTileResourcesForTesting(const std::vector<Tile*>& tiles) {
    for (size_t i = 0; i < tiles.size(); ++i) {
      Tile* tile = tiles[i];
      FreeResourcesForTile(tile);
    }
  }

  void SetGlobalStateForTesting(
      const GlobalStateThatImpactsTilePriority& state) {
    global_state_ = state;
  }

  void SetTileTaskManagerForTesting(
      std::unique_ptr<TileTaskManager> tile_task_manager) {
    tile_task_manager_ = std::move(tile_task_manager);
  }

  void SetRasterBufferProviderForTesting(
      RasterBufferProvider* raster_buffer_provider) {
    raster_buffer_provider_ = raster_buffer_provider;
  }

  std::vector<Tile*> AllTilesForTesting() const {
    std::vector<Tile*> tiles;
    for (auto& tile_pair : tiles_)
      tiles.push_back(tile_pair.second);
    return tiles;
  }

  void SetScheduledRasterTaskLimitForTesting(size_t limit) {
    scheduled_raster_task_limit_ = limit;
  }

  void CheckIfMoreTilesNeedToBePreparedForTesting() {
    CheckIfMoreTilesNeedToBePrepared();
  }

  void SetMoreTilesNeedToBeRasterizedForTesting() {
    all_tiles_that_need_to_be_rasterized_are_scheduled_ = false;
  }

  void ResetSignalsForTesting();

  bool HasScheduledTileTasksForTesting() const {
    return has_scheduled_tile_tasks_;
  }

  void OnRasterTaskCompleted(std::unique_ptr<RasterBuffer> raster_buffer,
                             Tile::Id tile_id,
                             Resource* resource,
                             bool was_canceled);

  void SetDecodedImageTracker(DecodedImageTracker* decoded_image_tracker);

  // CheckerImageTrackerClient implementation.
  void NeedsInvalidationForCheckerImagedTiles() override;

  // This method can only be used for debugging information, since it performs a
  // non trivial amount of work.
  std::unique_ptr<base::trace_event::ConvertableToTraceFormat>
  ActivationStateAsValue();

  void ActivationStateAsValueInto(base::trace_event::TracedValue* state);
  int num_of_tiles_with_checker_images() const {
    return num_of_tiles_with_checker_images_;
  }

  CheckerImageTracker& checker_image_tracker() {
    return checker_image_tracker_;
  }

 protected:
  friend class Tile;
  // Must be called by tile during destruction.
  void Release(Tile* tile);
  Tile::Id GetUniqueTileId() { return ++next_tile_id_; }

 private:
  class MemoryUsage {
   public:
    MemoryUsage();
    MemoryUsage(size_t memory_bytes, size_t resource_count);

    static MemoryUsage FromConfig(const gfx::Size& size,
                                  viz::ResourceFormat format);
    static MemoryUsage FromTile(const Tile* tile);

    MemoryUsage& operator+=(const MemoryUsage& other);
    MemoryUsage& operator-=(const MemoryUsage& other);
    MemoryUsage operator-(const MemoryUsage& other);

    bool Exceeds(const MemoryUsage& limit) const;
    int64_t memory_bytes() const { return memory_bytes_; }

   private:
    int64_t memory_bytes_;
    int resource_count_;
  };

  struct Signals {
    Signals();

    void reset();

    bool ready_to_activate;
    bool did_notify_ready_to_activate;
    bool ready_to_draw;
    bool did_notify_ready_to_draw;
    bool all_tile_tasks_completed;
    bool did_notify_all_tile_tasks_completed;
  };

  struct PrioritizedWorkToSchedule {
    PrioritizedWorkToSchedule();
    PrioritizedWorkToSchedule(PrioritizedWorkToSchedule&& other);
    ~PrioritizedWorkToSchedule();

    std::vector<PrioritizedTile> tiles_to_raster;
    std::vector<PrioritizedTile> tiles_to_process_for_images;
    CheckerImageTracker::ImageDecodeQueue checker_image_decode_queue;
  };

  void FreeResourcesForTile(Tile* tile);
  void FreeResourcesForTileAndNotifyClientIfTileWasReadyToDraw(Tile* tile);
  scoped_refptr<TileTask> CreateRasterTask(
      const PrioritizedTile& prioritized_tile,
      const gfx::ColorSpace& color_space,
      CheckerImageTracker::ImageDecodeQueue* checker_image_decode_queue);

  std::unique_ptr<EvictionTilePriorityQueue>
  FreeTileResourcesUntilUsageIsWithinLimit(
      std::unique_ptr<EvictionTilePriorityQueue> eviction_priority_queue,
      const MemoryUsage& limit,
      MemoryUsage* usage);
  std::unique_ptr<EvictionTilePriorityQueue>
  FreeTileResourcesWithLowerPriorityUntilUsageIsWithinLimit(
      std::unique_ptr<EvictionTilePriorityQueue> eviction_priority_queue,
      const MemoryUsage& limit,
      const TilePriority& oother_priority,
      MemoryUsage* usage);
  bool TilePriorityViolatesMemoryPolicy(const TilePriority& priority);
  bool AreRequiredTilesReadyToDraw(RasterTilePriorityQueue::Type type) const;
  void CheckIfMoreTilesNeedToBePrepared();
  void CheckAndIssueSignals();
  void MarkTilesOutOfMemory(
      std::unique_ptr<RasterTilePriorityQueue> queue) const;

  viz::ResourceFormat DetermineResourceFormat(const Tile* tile) const;
  bool DetermineResourceRequiresSwizzle(const Tile* tile) const;

  void DidFinishRunningTileTasksRequiredForActivation();
  void DidFinishRunningTileTasksRequiredForDraw();
  void DidFinishRunningAllTileTasks();

  scoped_refptr<TileTask> CreateTaskSetFinishedTask(
      void (TileManager::*callback)());
  PrioritizedWorkToSchedule AssignGpuMemoryToTiles();
  void ScheduleTasks(PrioritizedWorkToSchedule work_to_schedule);

  void PartitionImagesForCheckering(const PrioritizedTile& prioritized_tile,
                                    const gfx::ColorSpace& raster_color_space,
                                    std::vector<DrawImage>* sync_decoded_images,
                                    std::vector<PaintImage>* checkered_images);
  void AddCheckeredImagesToDecodeQueue(
      const PrioritizedTile& prioritized_tile,
      const gfx::ColorSpace& raster_color_space,
      CheckerImageTracker::DecodeType decode_type,
      CheckerImageTracker::ImageDecodeQueue* image_decode_queue);

  std::unique_ptr<base::trace_event::ConvertableToTraceFormat>
  ScheduledTasksStateAsValue() const;

  bool UsePartialRaster() const;

  void CheckPendingGpuWorkTiles(bool issue_signals);

  TileManagerClient* client_;
  base::SequencedTaskRunner* task_runner_;
  ResourcePool* resource_pool_;
  std::unique_ptr<TileTaskManager> tile_task_manager_;
  RasterBufferProvider* raster_buffer_provider_;
  GlobalStateThatImpactsTilePriority global_state_;
  size_t scheduled_raster_task_limit_;

  const TileManagerSettings tile_manager_settings_;
  bool use_gpu_rasterization_;

  std::unordered_map<Tile::Id, Tile*> tiles_;

  bool all_tiles_that_need_to_be_rasterized_are_scheduled_;
  MemoryHistory::Entry memory_stats_from_last_assign_;

  bool did_check_for_completed_tasks_since_last_schedule_tasks_;
  bool did_oom_on_last_assign_;

  ImageController image_controller_;
  CheckerImageTracker checker_image_tracker_;

  RasterTaskCompletionStats flush_stats_;

  TaskGraph graph_;

  UniqueNotifier more_tiles_need_prepare_check_notifier_;

  Signals signals_;

  UniqueNotifier signals_check_notifier_;

  bool has_scheduled_tile_tasks_;

  uint64_t prepare_tiles_count_;
  uint64_t next_tile_id_;

  std::unordered_set<Tile*> pending_gpu_work_tiles_;
  uint64_t pending_required_for_activation_callback_id_ = 0;
  uint64_t pending_required_for_draw_callback_id_ = 0;
  // If true, we should re-compute tile requirements in
  // CheckPendingGpuWorkTiles.
  bool pending_tile_requirements_dirty_ = false;

  std::unordered_map<Tile::Id, std::vector<DrawImage>> scheduled_draw_images_;
  std::vector<scoped_refptr<TileTask>> locked_image_tasks_;

  // Number of tiles with a checker-imaged resource or active raster tasks which
  // will create a checker-imaged resource.
  int num_of_tiles_with_checker_images_ = 0;

  // We need two WeakPtrFactory objects as the invalidation pattern of each is
  // different. The |task_set_finished_weak_ptr_factory_| is invalidated any
  // time new tasks are scheduled, preventing a race when the callback has
  // been scheduled but not yet executed.
  base::WeakPtrFactory<TileManager> task_set_finished_weak_ptr_factory_;
  // The |ready_to_draw_callback_weak_ptr_factory_| is never invalidated.
  base::WeakPtrFactory<TileManager> ready_to_draw_callback_weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(TileManager);
};

}  // namespace cc

#endif  // CC_TILES_TILE_MANAGER_H_
