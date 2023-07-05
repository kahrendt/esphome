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
 * it allocates enough memory for 2^32 aggregates.
 *
 * Time complexity (for n aggregate chunks, where each may aggregate multiple measurements):
 *  - insertion of new measurement: worst case log(n)
 *  - computing current aggregate: worst case log(n)
 *
 * Memory usage (for n aggregate chunks, where each may aggregate multiple measurements)::
 *  - log(n)+1 aggregates
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, Summer 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

namespace esphome {
namespace statistics {

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
