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
#include "aggregate_queue.h"
#include "circular_queue_index.h"

namespace esphome {
namespace statistics {

template<typename T> class DABALite : public AggregateQueue<T> {
 public:
  // Sets the window size by adjusting the capacity of the underlying circular queues
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t window_size, EnabledAggregatesConfiguration config) override;

  // Clears all readings from the sliding window
  void clear() override;

  // Number of measurements currently in the window
  size_t size() const override { return this->size_; }

  // Insert a value at end of circular queue and step the DABA Lite algorithm
  void insert(T value, uint32_t duration) override;
  void insert(Aggregate value) override;

  // Remove a value at start of circular queue and step the DABA Lite algorithm
  void evict() override;

  Aggregate compute_current_aggregate() override;

 protected:
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
