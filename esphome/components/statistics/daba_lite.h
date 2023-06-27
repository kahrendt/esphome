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

struct DABAEnabledAggregateConfiguration {
  bool count{false};
  bool max{false};
  bool min{false};
  bool mean{false};
  bool m2{false};
  bool timestamp_mean{false};
  bool c2{false};
  bool timestamp_m2{false};
};

class DABALite {
 public:
  DABALite(DABAEnabledAggregateConfiguration config, size_t capacity);

  // Sets the window size by adjusting the capacity of the underlying circular queues
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t window_size);

  bool get_memory_allocated() { return this->memory_allocated_; }

  // Clears all readings from the sliding window
  void clear();

  // Number of measurements currently in the window
  size_t size() const { return this->size_; }

  // Insert a value at end of circular queue and step the DABA Lite algorithm
  void insert(float value);

  // Remove a value at start of circular queue and step the DABA Lite algorithm
  void evict();

  Aggregate get_current_aggregate();

 protected:
  float *max_queue_{nullptr};
  float *min_queue_{nullptr};
  size_t *count_queue_{nullptr};
  float *mean_queue_{nullptr};
  float *m2_queue_{nullptr};
  float *c2_queue_{nullptr};
  float *timestamp_m2_queue_{nullptr};
  float *timestamp_mean_queue_{nullptr};
  uint32_t *timestamp_reference_queue_{nullptr};

  DABAEnabledAggregateConfiguration config_{};
  bool memory_allocated_{false};

  // Maximum window capacity
  size_t window_size_{0};

  // Number of measurements currently stored in window
  size_t size_{0};

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

  // Store an Aggregate at specified index only in the enabled queues
  void emplace_(const Aggregate &value, size_t index);

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
