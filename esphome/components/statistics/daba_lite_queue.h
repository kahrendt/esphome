/*
 * This class stores an array of aggregates for a sliding window of measurements/aggregate chunks. It has two main
 * functions:
 *  - Add a new aggregate chunk to sliding window queue (insert function)
 *  - Evict oldest aggregate chunk from the sliding window queue (evict function)
 *
 * This is used to aggregate continuously collected measurements into summary statistics over a sliding window. The
 * oldest measurements/aggregate chunks can be evicted and new measurements/aggregate chunks can be inserted. Its
 * calculations are numerically stable with minimal computational complexity. Each measurement/aggregate chunk in the
 * window is stored in memory along with two additional aggregates (mid_sum_ and back_sum_).
 *
 * The approach is based on the De-Amortized Banker's Aggregator (DABA) Lite algorithm. The code is based on
 * https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License, accessed June
 * 2023) and the reference paper "In-order sliding-window aggregation in worst-case constant time," by Kanat
 * Tangwongsan, Martin Hirzel, and Scott Schneider (https://doi.org/10.1007/s00778-021-00668-3).
 *
 * The measurements/aggregate chunks are stored in a circular queue that is allocated in memory in advance. The DABA
 * Lite algorithm keeps track of 6 indices, and the CircularQueueIndex class implements the details.
 *
 * Time complexity (for n aggregate chunks, where each may aggregate multiple measurements):
 *  - insertion of new measurement/aggregate chunk: O(1)
 *  - evicting oldest measurement/aggreate chunk: O(1)
 *  - clearing entire queue: O(1)
 *  - computing current aggregate: O(1)
 *
 * Memory usage (for n aggregate chunks, where each may aggregate multiple measurements):
 *  - n+2 aggregates
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
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
  /** Construct a default CircularQueueIndex.
   *
   * <index_> and <capacity_> are both 0
   */
  CircularQueueIndex();

  /** Construct a CircularQueueIndex.
   *
   * @param index Initial index number
   * @param capacity Size of the queue
   */
  CircularQueueIndex(size_t index, size_t capacity);

  void set_index(size_t index) { this->index_ = index; }
  size_t get_index() const { return this->index_; }

  size_t get_capacity() const { return this->capacity_; }

  /** Overloaded prefix increment operator
   *
   * Increases the index to point at the next item in the queue.
   */
  CircularQueueIndex &operator++();

  /** Overloaded prefix decrement operator
   *
   * Decreases the index to point at the previous item in the queue.
   */
  CircularQueueIndex &operator--();

  /** Overloaded equality operator
   *
   * Sets index_ and capacity_ equal to <rhs>'s values.
   */
  CircularQueueIndex &operator=(const CircularQueueIndex &rhs);

  /** Overloaded equality comparison operator
   *
   * @return true if index_ and capacity_ are equivalent to <rhs>'s values, false otherwise
   */
  bool operator==(const CircularQueueIndex &rhs) const;

  /** Overloaded inequality comparison operator
   *
   * @return true if index_ and capacity_ are not equivalent to <rhs>'s values, false otherwise
   */
  bool operator!=(const CircularQueueIndex &rhs) const;

 private:
  size_t index_;     // index of value in circular queue
  size_t capacity_;  // capacity of the circular queue
};

class DABALiteQueue : public AggregateQueue {
 public:
  //////////////////////////////////////////////////////////
  // Overridden virtual methods from AggregateQueue class //
  //////////////////////////////////////////////////////////

  /** Compute the aggregate summarizing all entries in the queue.
   *
   * Follows DABA Lite algorithm to compute the summary statistics of all aggregates in the queue.
   * @return the aggregate summary of all elements in the queue
   */
  Aggregate compute_current_aggregate() override;

  /** Clear all aggregates in the queue.
   *
   * All inserted aggregates are removed and the queue only stores the null measurement.
   */
  void clear() override;

  /** Remove the oldest value in the queue.
   *
   * The oldest aggregate is removed from the queue, and the DABA Lite algorithm steps to update running aggregates.
   */
  void evict() override;

  /** Insert aggregate at end of queue.
   *
   * A new aggregate is added to the queue, and the DABA Lite algorithm steps to update running aggregates.
   * @param value the aggregate to be inserted
   */
  void insert(Aggregate value) override;

  /** Set the queue's size and preallocates memory.
   *
   * @param window_size the total amount of aggregates that can be inserted into the queue
   * @param config which summary statistics should be saved into the queue
   * @return true if memory was sucecessfuly allocated, false otherwise
   */
  bool set_capacity(size_t window_size, EnabledAggregatesConfiguration config) override;

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

  //////////////////////////////////////////////
  // Internal Methods for DABA Lite algorithm //
  //////////////////////////////////////////////

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
