/*
 * This class stores an array of aggregates, which combined give summary statistics for all measurements inserted.
 * It has two main functions
 *  - Combine with a new aggregate (insert function)
 *  - Reset to a null aggregate (clear function)
 *
 * This is used to aggregate continuously collected measurements into summary statistics. Aggregates in the array
 * are combined when they aggregate the same number of measurements. As aggregates are only combined with same size
 * samples, this approach is numerically stable for any quantity of measurements. It has a small penalty in terms of
 * computational complexity and memory usage. Memory is allocated at the start. If no specific capacity is set,
 * it allocates enough memory for 2^(QUEUE_CAPACITY_IF_NONE_SPECIFIED)) aggregates to be inserted. If that is exceeded,
 * then an overflow is handled by aggregating everything in the queue into one aggregate. If the overflow handling
 * happens repeatedly, the aggregates can become numerically unstable.
 *
 *
 * Example run: At each numbered step, a new aggregate with count=1 is inserted. queue_ is the set of aggregates in the
 * queue, and each aggregates is denoted by its count.
 *
 *  1) queue_ = {1}
 *
 *  2) queue_ = {1,1}
 *     queue_ = {2}     // right most elements have same count, so they are combined
 *
 *  3) queue_ = {2,1}
 *
 *  4) queue_ = {2,1,1}
 *     queue_ = {2,2}   // right most elements have same count, so they are combined
 *     queue_ = {4}     // right most elements have same count, so they are combined
 *
 *  5) queue_ = {4,1}
 *
 *  6) queue_ = {4,1,1}
 *     queue_ = {4,2}   // right most elements have same count, so they are combined
 *
 *  7) queue_ = {4,2,1}
 *
 *  8) queue_ = {4,2,1,1}
 *     queue_ = {4,2,2} // right most elements have same count, so they are combined
 *     queue_ = {4,4}   // right most elements have same count, so they are combined
 *     queue_ = {8}     // right most elements have same count, so they are combined
 *
 *
 * Time complexity (for n aggregate chunks, where each may aggregate multiple measurements):
 *  - insertion of new measurement: worst case O(log(n))
 *  - clear queue: O(1)
 *  - computing current aggregate: worst case O(log(n))
 *
 * Memory usage (for n aggregate chunks, where each may aggregate multiple measurements):
 *  - log(n)+1 aggregates
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

namespace esphome {
namespace statistics {

// If no capacity is specified, we alloce memory for this many aggregates in our queue.
// If the queue is going to overflow, then the entire queue is aggregated into one aggregate.
//    - If this repeatedly occurs, then the statistics may not be numerically stable.
// The queue can hold 2^(QUEUE_CAPACITY_IF_NONE_SPECIFIED) aggregates before the overflow handling occurs.
static const uint8_t QUEUE_CAPACITY_IF_NONE_SPECIFIED = 32;

class RunningQueue : public AggregateQueue {
 public:
  // Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) override;

  // Clears all aggregates in the queue and resets
  void clear() override;
  void evict() override { this->clear(); }

  // Insert a value at end of the queue and consolidates if necessary
  void insert(Aggregate value) override;

  // Computes the summary statistics for all measurements stored in the queue
  Aggregate compute_current_aggregate() override;

 protected:
  size_t max_index_;

  // Stores the index one past the most recently inserted aggregate chunk
  uint8_t index_{0};

  // Returns the most recent aggregate chunk stored in the queue
  inline Aggregate get_end_();
};

}  // namespace statistics
}  // namespace esphome
