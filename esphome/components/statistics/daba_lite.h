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

namespace esphome {
namespace statistics {

/*
 * Class that handles an index for a circular queue
 *  - Circular queue concept:
 *    - Has a capacity set at the start
 *    - The index of the next element past the end of the array structure
 *       is the first index of the array structure; i.e., it loops around
 *    - An example implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326
 *  - Overloads operators to handle index operations respecting the circular queue structure
 *  - Should work on any array like structure with element access
 *
 */
class CircularQueueIndex {
 public:
  // Default constructor
  CircularQueueIndex();

  // General constructor
  CircularQueueIndex(size_t index, size_t capacity);

  void set_index(size_t index) { this->index_ = index; }
  size_t get_index() const { return this->index_; }

  void set_capacity(size_t capacity) { this->capacity_ = capacity; }
  size_t get_capacity() const { return this->capacity_; }

  // Overloaded prefix increment operator
  CircularQueueIndex &operator++();

  // Overloaded prefix decrement operator
  CircularQueueIndex &operator--();

  // Overloaded equality operator
  CircularQueueIndex &operator=(const CircularQueueIndex &i);

  // Overloaded equality comparison operator
  bool operator==(const CircularQueueIndex &i) const;

  // Overloaded inequality comparison operator
  bool operator!=(const CircularQueueIndex &i) const;

 private:
  size_t index_;     // index of value in circular queue
  size_t capacity_;  // capacity of the circular queue
};

class DABALite : public AggregateQueue {
 public:
  // Sets the window size by adjusting the capacity of the underlying circular queues
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t window_size, EnabledAggregatesConfiguration config) override;

  // Clears all readings from the sliding window
  void clear() override;

  // Insert a value at end of circular queue and step the DABA Lite algorithm
  void insert(Aggregate value) override;

  // Remove a value at start of circular queue and step the DABA Lite algorithm
  void evict() override;

  Aggregate compute_current_aggregate() override;

 protected:
  // Maximum window capacity
  size_t window_size_{0};

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
