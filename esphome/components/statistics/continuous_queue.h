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
 * then an overflow is handled by aggregating everything in the queue into one aggregate. Repeated overflow handling
 * can cause numerical instability.
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
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

namespace esphome {
namespace statistics {

// If no capacity is specified, we allocate memory for this amount of stored aggregates in our queue.
// The queue can insert 2^(QUEUE_CAPACITY_IF_NONE_SPECIFIED) aggregates before the overflow handling occurs.
static const uint8_t QUEUE_CAPACITY_IF_NONE_SPECIFIED = 32;

class ContinuousQueue : public AggregateQueue {
 public:
  //////////////////////////////////////////////////////////
  // Overridden virtual methods from AggregateQueue class //
  //////////////////////////////////////////////////////////

  /** Compute the aggregate summarizing all entries in the queue.
   *
   * Compute the summary statistics of all aggregates in the queue by combining them.
   * @return the aggregate summary of all elements in the queue
   */
  Aggregate compute_current_aggregate() override;

  /// @brief Clear all aggregates in the queue.
  void clear() override;

  /// @brief Equivalent to clearing all aggregates in the queue
  void evict() override { this->clear(); }

  /** Insert aggregate at end of queue.
   *
   * A new aggregate is added to the queue, and consolidates previous queue elements if necessary
   * @param value the aggregate to be inserted
   */
  void insert(Aggregate value) override;

  /** Set the queue's size and preallocates memory.
   *
   * This queue uses at most log_2(<window_size>)+1 aggregates to store <chunk_capacity> aggregate chunks.
   * @param chunk_capacity the total amount of aggregate chunks that can be inserted into the queue
   * @param config which summary statistics should be saved into the queue
   * @return true if memory was sucecessfuly allocated, false otherwise
   */
  bool set_capacity(size_t chunk_capacity, EnabledAggregatesConfiguration enabled_config) override;

 protected:
  // Largest possible index before running out of memory
  size_t max_index_;

  // Stores the index one past the most recently inserted aggregate chunk
  uint8_t index_{0};

  //////////////////////////////////////////
  // Internal Methods for Continous Queue //
  //////////////////////////////////////////

  /// @brief Returns the most recent aggregate chunk stored in the queue
  inline Aggregate get_end_();
};

}  // namespace statistics
}  // namespace esphome
