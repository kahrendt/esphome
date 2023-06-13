/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)

  Implemented by Kevin Ahrendt, June 2023
*/

#pragma once

#include "circular_queue_index.h"
#include "aggregate.h"

#include "esphome/core/helpers.h"

#include <vector>

namespace esphome {
namespace statistics {

class DABALite {
 public:
  // each store the current aggregate and return the desired summary statistic
  //  - avoids recomputing the aggregate if it is still current
  float aggregated_count();
  float aggregated_max();
  float aggregated_min();
  float aggregated_mean();
  float aggregated_variance();
  float aggregated_std_dev();
  float aggregated_covariance();
  float aggregated_trend();

  // sets the window size by adjusting the capacity of the underlying circular queues
  void set_capacity(size_t window_size);

  // number of measurements currently in the window
  size_t size() const { return this->size_; }

  // insert value at end of circular queue and step DABA Lite algorithm
  void insert(float value);

  // remove value at start of circular queue and step DABA Lite algorithm
  void evict();

  // enable storing values for these aggregates
  void enable_count() { this->include_count_ = true; }
  void enable_max() { this->include_max_ = true; }
  void enable_min() { this->include_min_ = true; }
  void enable_mean() { this->include_mean_ = true; }
  void enable_m2() { this->include_m2_ = true; }
  void enable_timestamp_mean() { this->include_timestamp_mean_ = true; }
  void enable_c2() { this->include_c2_ = true; }
  void enable_timestamp_m2() { this->include_timestamp_m2_ = true; }

 protected:
  // Vectors for storing summary statistics
  //    - allocated to PSRam if available
  std::vector<float, ExternalRAMAllocator<float>> max_queue_{};
  std::vector<float, ExternalRAMAllocator<float>> min_queue_{};
  std::vector<size_t, ExternalRAMAllocator<float>> count_queue_{};
  std::vector<float, ExternalRAMAllocator<float>> mean_queue_{};
  std::vector<float, ExternalRAMAllocator<float>> m2_queue_{};
  std::vector<float, ExternalRAMAllocator<float>> c2_queue_{};
  std::vector<float, ExternalRAMAllocator<float>> timestamp_m2_queue_{};

  std::vector<int32_t, ExternalRAMAllocator<int32_t>> timestamp_sum_queue_{};
  std::vector<uint32_t, ExternalRAMAllocator<uint32_t>> timestamp_reference_queue_{};

  bool include_count_{false};
  bool include_max_{false};
  bool include_min_{false};
  bool include_mean_{false};
  bool include_m2_{false};
  bool include_timestamp_mean_{false};
  bool include_c2_{false};
  bool include_timestamp_m2_{false};

  void debug_pointers_();

  // maximum window capacity
  size_t window_size_{0};

  // number of measurements stored in window
  size_t size_{0};

  // if we have the current aggregate, we do not query it again but just use the stored result
  bool is_current_aggregate_updated_{false};
  Aggregate current_aggregate_;

  // Update current_aggregate_ to account for latest changes
  void update_current_aggregate_();

  // DABA Lite - Raw Indices for queue_; i.e., not offset by the head index
  CircularQueueIndex f_;  // front of queue
  CircularQueueIndex l_;
  CircularQueueIndex r_;
  CircularQueueIndex a_;
  CircularQueueIndex b_;
  CircularQueueIndex e_;  // end of queue (one past the most recently inserted measurement)

  // default values for a null measurement
  const Aggregate identity_class_;

  // running aggregates for DABA Lite algorithm
  Aggregate mid_sum_, back_sum_;

  // compute summary statistics for a single measurement and returns them as an Aggregate
  Aggregate lift_(float v);

  // store an Aggregate at an index only in the enabled queue_ vectors
  void emplace_(const Aggregate &value, size_t index);

  // combine summary statistics from two Aggregates
  Aggregate combine_(const Aggregate &a, const Aggregate &b);

  // return summary statistics at given index as an Aggregate
  Aggregate lower_(size_t index);

  // DABA Lite algorithm method
  void step_();
  // DABA Lite algorithm method
  void flip_();

  // DABA Lite algorithm methods
  inline bool is_front_empty_();
  inline bool is_delta_empty_();
  inline Aggregate get_back_();
  inline Aggregate get_alpha_();
  inline Aggregate get_delta_();
};

}  // namespace statistics
}  // namespace esphome
