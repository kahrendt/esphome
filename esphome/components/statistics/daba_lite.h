/*
 * Sliding window aggregates are stored and computed using the De-Amortized Banker's Aggregator Lite (DABA Lite)
 * algorithm
 *  - space requirements: n+2 aggregates
 *  - time complexity: worse-case O(1)
 *  - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"
#include "circular_queue_index.h"

namespace esphome {
namespace statistics {

class DABALite {
 public:
  // Each function update the current aggregate and return the desired summary statistic
  //  - Avoids updating the aggregate if it is still current
  float aggregated_count();
  float aggregated_max();
  float aggregated_min();
  float aggregated_mean();
  float aggregated_variance();
  float aggregated_std_dev();
  float aggregated_covariance();
  float aggregated_trend();

  // Sets the window size by adjusting the capacity of the underlying circular queues
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t window_size);

  // Clears all readings from the sliding window
  void clear();

  // Number of measurements currently in the window
  size_t size() const { return this->size_; }

  // Insert a value at end of circular queue and step the DABA Lite algorithm
  void insert(float value);

  // Remove a value at start of circular queue and step the DABA Lite algorithm
  void evict();

  // Each function enables storing the aggregates for their respective values
  void enable_count() { this->include_count_ = true; }
  void enable_max() { this->include_max_ = true; }
  void enable_min() { this->include_min_ = true; }
  void enable_mean() { this->include_mean_ = true; }
  void enable_m2() { this->include_m2_ = true; }
  void enable_timestamp_mean() { this->include_timestamp_mean_ = true; }
  void enable_c2() { this->include_c2_ = true; }
  void enable_timestamp_m2() { this->include_timestamp_m2_ = true; }

 protected:
  float *max_queue_{nullptr};
  float *min_queue_{nullptr};
  size_t *count_queue_{nullptr};
  float *mean_queue_{nullptr};
  float *m2_queue_{nullptr};
  float *c2_queue_{nullptr};
  float *timestamp_m2_queue_{nullptr};
  int32_t *timestamp_sum_queue_{nullptr};
  uint32_t *timestamp_reference_queue_{nullptr};

  bool include_count_{false};
  bool include_max_{false};
  bool include_min_{false};
  bool include_mean_{false};
  bool include_m2_{false};
  bool include_timestamp_mean_{false};
  bool include_c2_{false};
  bool include_timestamp_m2_{false};

  // Maximum window capacity
  size_t window_size_{0};

  // Number of measurements currently stored in window
  size_t size_{0};

  // If the current aggregate is updated, it is not repeatedly updates but instead stored
  bool is_current_aggregate_updated_{false};
  Aggregate current_aggregate_;

  // Update current_aggregate_ to account for latest changes (necessary after an insertion or evict operation)
  void update_current_aggregate_();

  // DABA Lite - raw Indices for queues; i.e., not offset by the head index
  CircularQueueIndex f_;  // front of queue
  CircularQueueIndex l_;
  CircularQueueIndex r_;
  CircularQueueIndex a_;
  CircularQueueIndex b_;
  CircularQueueIndex e_;  // end of queue (one past the most recently inserted measurement)

  // Default values for an empty set of measurements
  const Aggregate identity_class_;

  // Running aggregates for DABA Lite algorithm
  Aggregate mid_sum_, back_sum_;

  // Compute aggregates for a single measurement v and return it as an Aggregate
  Aggregate lift_(float v);

  // Store an Aggregate at specified index only in the enabled queues
  void emplace_(const Aggregate &value, size_t index);

  // Combine Aggregates for two disjoint sets of measurements
  Aggregate combine_(const Aggregate &a, const Aggregate &b);

  // Return aggregates at a given index in the queues
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
