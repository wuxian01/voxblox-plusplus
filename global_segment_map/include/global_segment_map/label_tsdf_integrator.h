#ifndef GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_
#define GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_

#include <algorithm>
#include <list>
#include <map>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <voxblox/core/layer.h>
#include <voxblox/core/voxel.h>
#include <voxblox/integrator/integrator_utils.h>
#include <voxblox/integrator/tsdf_integrator.h>
#include <voxblox/utils/timing.h>

#include "global_segment_map/label_tsdf_map.h"
#include "global_segment_map/label_voxel.h"

namespace voxblox {

class Segment {
 public:
  voxblox::Pointcloud points_C_;
  voxblox::Transformation T_G_C_;
  voxblox::Colors colors_;
  voxblox::Labels labels_;
};

class LabelTsdfIntegrator : public MergedTsdfIntegrator {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  typedef AnyIndexHashMapType<AlignedVector<size_t>>::type VoxelMap;

  struct LabelTsdfConfig {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    bool enable_pairwise_confidence_merging = false;
    float pairwise_confidence_ratio_threshold = 0.05f;
    int pairwise_confidence_threshold = 2;

    // Experiments showed that capped confidence value
    // only introduces artifacts in planar regions.
    // Cap confidence settings.
    bool cap_confidence = false;
    int confidence_cap_value = 10;
  };

  LabelTsdfIntegrator(const Config& config,
                      const LabelTsdfConfig& label_tsdf_config,
                      Layer<TsdfVoxel>* tsdf_layer,
                      Layer<LabelVoxel>* label_layer, Label* highest_label)
      : MergedTsdfIntegrator(config, CHECK_NOTNULL(tsdf_layer)),
        label_tsdf_config_(label_tsdf_config),
        label_layer_(CHECK_NOTNULL(label_layer)),
        highest_label_(CHECK_NOTNULL(highest_label)) {
    CHECK(label_layer_);
  }

  inline void checkForSegmentLabelMergeCandidate(
      Label label, int label_points_count, int segment_points_count,
      std::unordered_set<Label>* merge_candidate_labels) {
    // All segment labels that overlap with more than a certain
    // percentage of the segment points are potential merge candidates.
    float label_segment_overlap_ratio =
        static_cast<float>(label_points_count) /
        static_cast<float>(segment_points_count);
    if (label_segment_overlap_ratio >
        label_tsdf_config_.pairwise_confidence_ratio_threshold) {
      merge_candidate_labels->insert(label);
    }
  }

  inline void increaseLabelCountForSegment(
      Segment* segment, Label label, int segment_points_count,
      std::map<Label, std::map<Segment*, size_t>>* candidates,
      std::unordered_set<Label>* merge_candidate_labels) {
    auto label_it = candidates->find(label);
    if (label_it != candidates->end()) {
      auto segment_it = label_it->second.find(segment);
      if (segment_it != label_it->second.end()) {
        ++segment_it->second;

        if (label_tsdf_config_.enable_pairwise_confidence_merging) {
          checkForSegmentLabelMergeCandidate(label, segment_it->second,
                                             segment_points_count,
                                             merge_candidate_labels);
        }
      } else {
        label_it->second.emplace(segment, 1u);
      }
    } else {
      std::map<Segment*, size_t> segment_points_count;
      segment_points_count.emplace(segment, 1u);
      candidates->emplace(label, segment_points_count);
    }
  }

  inline void increasePairwiseConfidenceCount(
      std::vector<Label> merge_candidates) {
    // For every couple of labels from the merge candidates
    // set or increase their pairwise confidence.
    for (size_t i = 0u; i < merge_candidates.size(); ++i) {
      for (size_t j = i + 1; j < merge_candidates.size(); ++j) {
        Label label1 = merge_candidates[i];
        Label label2 = merge_candidates[j];

        if (label1 != label2) {
          // Pairs consist of (label1, label2) where label1 < label2.
          if (label1 > label2) {
            Label tmp = label2;
            label2 = label1;
            label1 = tmp;
          }
          auto pairs = pairwise_confidence_.find(label1);
          if (pairs != pairwise_confidence_.end()) {
            auto confidence = pairs->second.find(label2);
            if (confidence != pairs->second.end()) {
              ++confidence->second;
            } else {
              pairs->second.emplace(label2, 1);
            }
          } else {
            std::map<Label, int> confidence_pair;
            confidence_pair.emplace(label2, 1);
            pairwise_confidence_.emplace(label1, confidence_pair);
          }
        }
      }
    }
  }

  inline void computeSegmentLabelCandidates(
      Segment* segment,
      std::map<Label, std::map<Segment*, size_t>>* candidates) {
    DCHECK(segment != nullptr);
    DCHECK(candidates != nullptr);
    // Flag to check whether there exists at least one label candidate.
    bool candidate_label_exists = false;
    const int segment_points_count = segment->points_C_.size();
    std::unordered_set<Label> merge_candidate_labels;

    for (const Point& point_C : segment->points_C_) {
      const Point point_G = segment->T_G_C_ * point_C;

      // Get the corresponding voxel by 3D position in world frame.
      Layer<LabelVoxel>::BlockType::ConstPtr block_ptr =
          label_layer_->getBlockPtrByCoordinates(point_G);

      if (block_ptr != nullptr) {
        const LabelVoxel& voxel = block_ptr->getVoxelByCoordinates(point_G);
        // Do not consider allocated but unobserved voxels
        // which have label == 0.
        if (voxel.label != 0u) {
          candidate_label_exists = true;
          increaseLabelCountForSegment(segment, voxel.label,
                                       segment_points_count, candidates,
                                       &merge_candidate_labels);
        }
      }
    }

    if (label_tsdf_config_.enable_pairwise_confidence_merging) {
      std::vector<Label> merge_candidates;
      std::copy(merge_candidate_labels.begin(), merge_candidate_labels.end(),
                std::back_inserter(merge_candidates));

      increasePairwiseConfidenceCount(merge_candidates);
    }

    // Previously unobserved segment gets an unseen label.
    if (!candidate_label_exists) {
      Label fresh_label = getFreshLabel();
      std::map<Segment*, size_t> map;
      map.insert(
          std::pair<Segment*, size_t>(segment, segment->points_C_.size()));
      candidates->insert(
          std::pair<Label, std::map<Segment*, size_t>>(fresh_label, map));
    }
  }

  // Fetch the next segment label pair which has overall
  // the highest voxel count.
  inline bool getNextSegmentLabelPair(
      std::map<voxblox::Label, std::map<voxblox::Segment*, size_t>>* candidates,
      std::set<Segment*>* labelled_segments,
      std::pair<Segment*, Label>* segment_label_pair) {
    Label max_label;
    size_t max_count = 0u;
    Segment* max_segment;

    for (auto label_it = candidates->begin(); label_it != candidates->end();
         ++label_it) {
      for (auto segment_it = label_it->second.begin();
           segment_it != label_it->second.end(); segment_it++) {
        if (segment_it->second > max_count &&
            labelled_segments->find(segment_it->first) ==
                labelled_segments->end()) {
          max_segment = segment_it->first;
          max_count = segment_it->second;
          max_label = label_it->first;
        }
      }
    }
    if (max_count == 0u) {
      return false;
    }
    segment_label_pair->first = max_segment;
    segment_label_pair->second = max_label;

    return true;
  }

  inline void decideLabelPointClouds(
      std::vector<voxblox::Segment*>* segments_to_integrate,
      std::map<voxblox::Label, std::map<voxblox::Segment*, size_t>>*
          candidates) {
    std::set<Segment*> labelled_segments;
    std::pair<Segment*, Label> pair;

    while (getNextSegmentLabelPair(candidates, &labelled_segments, &pair)) {
      for (size_t i = 0u; i < pair.first->points_C_.size(); ++i) {
        pair.first->labels_.push_back(pair.second);
      }
      labelled_segments.insert(pair.first);
      candidates->erase(pair.second);
    }

    // For every segment that didn't get a label, assign it an unseen label.
    for (auto segment_it = segments_to_integrate->begin();
         segment_it != segments_to_integrate->end(); ++segment_it) {
      if (labelled_segments.find(*segment_it) == labelled_segments.end()) {
        Label fresh = getFreshLabel();
        for (size_t i = 0u; i < (*segment_it)->points_C_.size(); ++i) {
          (*segment_it)->labels_.push_back(fresh);
        }
        labelled_segments.insert(*segment_it);
      }
    }
  }

  // TODO(grinvalm): find a way to do bookkeping of the
  // voxel counts in a multithreaded scenario
  // without blocking the parallelism with mutexes.

  //  // Increase or decrease the voxel count for a label.
  //  // TODO(grinvalm): when count for a label goes to 0
  //  // remove label from label_count.
  //  inline void changeLabelCount(const Label label, int count,
  //                               std::lock_guard<std::mutex> lock) {
  //    auto label_count = labels_count_map_.find(label);
  //    if (label_count != labels_count_map_.end()) {
  //      label_count->second = label_count->second + count;
  //    } else {
  //      if (label != 0u) {
  //        labels_count_map_.insert(std::make_pair(label, count));
  //      }
  //    }
  //  }

  // Will return a pointer to a voxel located at global_voxel_idx in the tsdf
  // layer. Thread safe.
  // Takes in the last_block_idx and last_block to prevent unneeded map lookups.
  // If the block this voxel would be in has not been allocated, a block in
  // temp_block_map_ is created/accessed and a voxel from this map is returned
  // instead. Unlike the layer, accessing temp_block_map_ is controlled via a
  // mutex allowing it to grow during integration.
  // These temporary blocks can be merged into the layer later by calling
  // updateLayerWithStoredBlocks()
  LabelVoxel* allocateStorageAndGetLabelVoxelPtr(
      const VoxelIndex& global_voxel_idx, Block<LabelVoxel>::Ptr* last_block,
      BlockIndex* last_block_idx) {
    DCHECK(last_block != nullptr);
    DCHECK(last_block_idx != nullptr);

    const BlockIndex block_idx = getBlockIndexFromGlobalVoxelIndex(
        global_voxel_idx, voxels_per_side_inv_);

    if ((block_idx != *last_block_idx) || (*last_block == nullptr)) {
      *last_block = label_layer_->getBlockPtrByIndex(block_idx);
      *last_block_idx = block_idx;
    }

    // If no block at this location currently exists, we allocate a temporary
    // voxel that will be merged into the map later
    if (*last_block == nullptr) {
      // To allow temp_block_map_ to grow we can only let one thread in at once
      std::lock_guard<std::mutex> lock(temp_block_mutex_);

      typename Layer<LabelVoxel>::BlockHashMap::iterator it =
          temp_label_block_map_.find(block_idx);
      if (it != temp_label_block_map_.end()) {
        *last_block = it->second;
      } else {
        auto insert_status = temp_label_block_map_.emplace(
            block_idx,
            std::make_shared<Block<LabelVoxel>>(
                voxels_per_side_, voxel_size_,
                getOriginPointFromGridIndex(block_idx, block_size_)));

        DCHECK(insert_status.second)
            << "Block already exists when allocating at "
            << block_idx.transpose();

        *last_block = insert_status.first->second;
      }
    }

    (*last_block)->updated() = true;

    const VoxelIndex local_voxel_idx =
        getLocalFromGlobalVoxelIndex(global_voxel_idx, voxels_per_side_);

    return &((*last_block)->getVoxelByVoxelIndex(local_voxel_idx));
  }

  // NOT thread safe
  void updateLabelLayerWithStoredBlocks() {
    BlockIndex last_block_idx;
    Block<LabelVoxel>::Ptr block = nullptr;

    for (const std::pair<const BlockIndex, Block<LabelVoxel>::Ptr>&
             temp_label_block_pair : temp_label_block_map_) {
      label_layer_->insertBlock(temp_label_block_pair);
    }

    temp_block_map_.clear();
  }

  inline void updateLabelVoxel(const Point& point_G, const Label& label,
                               LabelVoxel* label_voxel) {
    updateLabelVoxel(point_G, label, 1u, label_voxel);
  }

  // Updates label_voxel. Thread safe.
  inline void updateLabelVoxel(const Point& point_G, const Label& label,
                               const LabelConfidence& confidence,
                               LabelVoxel* label_voxel) {
    // Lookup the mutex that is responsible for this voxel and lock it
    std::lock_guard<std::mutex> lock(
        mutexes_.get(getGridIndexFromPoint(point_G, voxel_size_inv_)));

    CHECK_NOTNULL(label_voxel);

    if (label_voxel->label == label) {
      label_voxel->label_confidence =
          label_voxel->label_confidence + confidence;
      if (label_tsdf_config_.cap_confidence) {
        if (label_voxel->label_confidence >
            label_tsdf_config_.confidence_cap_value) {
          label_voxel->label_confidence =
              label_tsdf_config_.confidence_cap_value;
        }
      }
    } else {
      if (label_voxel->label_confidence == 0u) {
        //        changeLabelCount(label, 1, lock);
        //        changeLabelCount(label_voxel->label, -1, lock);

        label_voxel->label = label;
        label_voxel->label_confidence = confidence;
        if (*highest_label_ < label) {
          *highest_label_ = label;
        }
      } else {
        label_voxel->label_confidence =
            label_voxel->label_confidence - confidence;
      }
    }
  }

  void integratePointCloud(const Transformation& T_G_C,
                           const Pointcloud& points_C, const Colors& colors,
                           const Labels& labels, const bool freespace_points) {
    DCHECK_EQ(points_C.size(), colors.size());

    timing::Timer integrate_timer("integrate");

    // Pre-compute a list of unique voxels to end on.
    // Create a hashmap: VOXEL INDEX -> index in original cloud.
    VoxelMap voxel_map;
    // This is a hash map (same as above) to all the indices that need to be
    // cleared.
    VoxelMap clear_map;

    ThreadSafeIndex index_getter(points_C.size());

    bundleRays(T_G_C, points_C, colors, freespace_points, &index_getter,
               &voxel_map, &clear_map);

    integrateRays(T_G_C, points_C, colors, labels, config_.enable_anti_grazing,
                  false, voxel_map, clear_map);

    timing::Timer clear_timer("integrate/clear");

    integrateRays(T_G_C, points_C, colors, labels, config_.enable_anti_grazing,
                  true, voxel_map, clear_map);

    clear_timer.Stop();

    integrate_timer.Stop();
  }

  void integrateVoxel(const Transformation& T_G_C, const Pointcloud& points_C,
                      const Colors& colors, const Labels& labels,
                      bool enable_anti_grazing, bool clearing_ray,
                      const std::pair<AnyIndex, AlignedVector<size_t>>& kv,
                      const VoxelMap& voxel_map) {
    if (kv.second.empty()) {
      return;
    }

    const Point& origin = T_G_C.getPosition();
    Color merged_color;
    Point merged_point_C = Point::Zero();
    FloatingPoint merged_weight = 0.0f;
    Label merged_label;

    for (const size_t pt_idx : kv.second) {
      const Point& point_C = points_C[pt_idx];
      const Color& color = colors[pt_idx];
      const Label& label = labels[pt_idx];

      const float point_weight = getVoxelWeight(point_C);
      merged_point_C =
          (merged_point_C * merged_weight + point_C * point_weight) /
          (merged_weight + point_weight);
      merged_color = Color::blendTwoColors(merged_color, merged_weight, color,
                                           point_weight);
      merged_weight += point_weight;
      // Assuming all the points of a segment pointcloud
      // are assigned the same label.
      merged_label = label;

      // only take first point when clearing
      if (clearing_ray) {
        break;
      }
    }

    const Point merged_point_G = T_G_C * merged_point_C;

    RayCaster ray_caster(origin, merged_point_G, clearing_ray,
                         config_.voxel_carving_enabled,
                         config_.max_ray_length_m, voxel_size_inv_,
                         config_.default_truncation_distance);

    VoxelIndex global_voxel_idx;
    while (ray_caster.nextRayIndex(&global_voxel_idx)) {
      if (enable_anti_grazing) {
        // Check if this one is already the block hash map for this
        // insertion. Skip this to avoid grazing.
        if ((clearing_ray || global_voxel_idx != kv.first) &&
            voxel_map.find(global_voxel_idx) != voxel_map.end()) {
          continue;
        }
      }
      BlockIndex block_idx;

      Block<TsdfVoxel>::Ptr tsdf_block = nullptr;
      TsdfVoxel* tsdf_voxel = allocateStorageAndGetVoxelPtr(
          global_voxel_idx, &tsdf_block, &block_idx);

      updateTsdfVoxel(origin, merged_point_G, global_voxel_idx, merged_color,
                      merged_weight, tsdf_voxel);

      Block<LabelVoxel>::Ptr label_block = nullptr;
      LabelVoxel* label_voxel = allocateStorageAndGetLabelVoxelPtr(
          global_voxel_idx, &label_block, &block_idx);

      updateLabelVoxel(merged_point_G, merged_label, 1u, label_voxel);
    }
  }

  void integrateVoxels(const Transformation& T_G_C, const Pointcloud& points_C,
                       const Colors& colors, const Labels& labels,
                       bool enable_anti_grazing, bool clearing_ray,
                       const VoxelMap& voxel_map, const VoxelMap& clear_map,
                       size_t thread_idx) {
    VoxelMap::const_iterator it;
    size_t map_size;
    if (clearing_ray) {
      it = clear_map.begin();
      map_size = clear_map.size();
    } else {
      it = voxel_map.begin();
      map_size = voxel_map.size();
    }
    for (size_t i = 0; i < map_size; ++i) {
      if (((i + thread_idx + 1) % config_.integrator_threads) == 0) {
        integrateVoxel(T_G_C, points_C, colors, labels, enable_anti_grazing,
                       clearing_ray, *it, voxel_map);
      }
      ++it;
    }
  }

  void integrateRays(
      const Transformation& T_G_C, const Pointcloud& points_C,
      const Colors& colors, const Labels& labels, bool enable_anti_grazing,
      bool clearing_ray,
      const VoxelMap& voxel_map,
      const VoxelMap& clear_map) {
    const Point& origin = T_G_C.getPosition();

    // if only 1 thread just do function call, otherwise spawn threads
    if (config_.integrator_threads == 1) {
      constexpr size_t thread_idx = 0;
      integrateVoxels(T_G_C, points_C, colors, labels, enable_anti_grazing,
                      clearing_ray, voxel_map, clear_map, thread_idx);
    } else {
      std::list<std::thread> integration_threads;
      for (size_t i = 0; i < config_.integrator_threads; ++i) {
        integration_threads.emplace_back(&LabelTsdfIntegrator::integrateVoxels,
                                         this, T_G_C, points_C, colors, labels,
                                         enable_anti_grazing, clearing_ray,
                                         voxel_map, clear_map, i);
      }

      for (std::thread& thread : integration_threads) {
        thread.join();
      }
    }

    timing::Timer insertion_timer("inserting_missed_blocks");
    updateLayerWithStoredBlocks();
    updateLabelLayerWithStoredBlocks();


    insertion_timer.Stop();
  }

  void swapLabels(Label old_label, Label new_label) {
    BlockIndexList all_label_blocks;
    label_layer_->getAllAllocatedBlocks(&all_label_blocks);

    for (const BlockIndex& block_index : all_label_blocks) {
      Block<LabelVoxel>::Ptr block =
          label_layer_->getBlockPtrByIndex(block_index);
      size_t vps = block->voxels_per_side();
      for (int i = 0; i < vps * vps * vps; i++) {
        LabelVoxel& voxel = block->getVoxelByLinearIndex(i);
        if (voxel.label == old_label) {
          voxel.label = new_label;
          //          changeLabelCount(new_label, 1);
          //          changeLabelCount(old_label, -1);

          block->updated() = true;
        }
      }
    }
  }

  void mergeLabels() {
    if (label_tsdf_config_.enable_pairwise_confidence_merging) {
      for (auto& confidence_map : pairwise_confidence_) {
        for (auto confidence_pair_it = confidence_map.second.begin();
             confidence_pair_it != confidence_map.second.end();
             ++confidence_pair_it) {
          if (confidence_pair_it->second >
              label_tsdf_config_.pairwise_confidence_threshold) {
            swapLabels(confidence_map.first, confidence_pair_it->first);
            LOG(ERROR) << "Merging labels " << confidence_map.first << " and "
                       << confidence_pair_it->first;
            confidence_map.second.erase(confidence_pair_it->first);
          }
        }
      }
    }
  }

  Label getFreshLabel() {
    CHECK_LT(*highest_label_, std::numeric_limits<unsigned int>::max());
    return ++(*highest_label_);
  }

  // Get the list of all labels
  // for which the voxel count is greater than 0.
  std::vector<Label> getLabelsList() {
    std::vector<Label> labels;
    for (auto& label_count_pair : labels_count_map_) {
      if (label_count_pair.second > 0) {
        labels.push_back(label_count_pair.first);
      }
    }
    return labels;
  }

 protected:
  LabelTsdfConfig label_tsdf_config_;
  Layer<LabelVoxel>* label_layer_;

  // Temporary block storage, used to hold blocks that need to be created while
  // integrating a new pointcloud
  std::mutex temp_block_mutex_;  // TODO(grinvalm): one mutex is enough for
                                 // label and tsdf?
  Layer<LabelVoxel>::BlockHashMap temp_label_block_map_;

  Label* highest_label_;
  std::map<Label, int> labels_count_map_;

  // Pairwise confidence merging settings.
  std::map<Label, std::map<Label, int>> pairwise_confidence_;

  // We need to prevent simultaneous access to the voxels in the map. We could
  // put a single mutex on the map or on the blocks, but as voxel updating is
  // the most expensive operation in integration and most voxels are close
  // together, both strategies would bottleneck the system. We could make a
  // mutex per voxel, but this is too ram heavy as one mutex = 40 bytes.
  // Because of this we create an array that is indexed by the first n bits of
  // the voxels hash. Assuming a uniform hash distribution, this means the
  // chance of two threads needing the same lock for unrelated voxels is
  // (num_threads / (2^n)). For 8 threads and 12 bits this gives 0.2%.
  ApproxHashArray<12, std::mutex> mutexes_;
};

}  // namespace voxblox

#endif  // GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_
