/*
 * This class stores a single running aggregate that can do two things:
 *  - Combine with a new aggregate (insert function)
 *  - Reset to a null aggregate (clear function)
 *
 * This is used to aggregate continuously collected measurements into summary statistics, but it may lose accuracy for
 * large sets of measurements
 *
 * Time complexity:
 *  - insertion of new measurement: O(1)
 *  - clear current aggregate: O(1)
 *  - computing current aggregate: O(1)
 *
 * Memory usage:
 *  - 1 aggregate for n measurement/chunks
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

namespace esphome {
namespace statistics {

class RunningSingular : public AggregateQueue {
 public:
  // No memory allocation is necessary in this singular running case, so always return true for success
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) override;

  // Resets the running aggregate
  void clear() override;
  void evict() override { this->clear(); }

  // Add another measurement into the running aggregate
  void insert(Aggregate value) override;

  // Returns the summary statistics for all aggregated measurements
  Aggregate compute_current_aggregate() override { return this->running_aggregate_; }

 protected:
  Aggregate running_aggregate_{};
};

}  // namespace statistics
}  // namespace esphome
