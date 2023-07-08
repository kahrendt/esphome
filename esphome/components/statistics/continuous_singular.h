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

class ContinuousSingular : public AggregateQueue {
 public:
  //////////////////////////////////////////////////////////
  // Overridden virtual methods from AggregateQueue class //
  //////////////////////////////////////////////////////////

  /// @brief Return the summary statistics for the running aggregate.
  /// @return the current running aggregate
  Aggregate compute_current_aggregate() override { return this->running_aggregate_; }

  /// @brief Clear all aggregates in the queue.
  void clear() override;

  /// @brief Equivalent to clearing all aggregates in the queue
  void evict() override { this->clear(); }

  /** Insert aggregate into running aggegrate.
   *
   * <value> is aggregated into <running_aggregate_>
   * @param value the aggregate to be inserted
   */
  void insert(Aggregate value) override;

  /// @brief No memory allocation is necessary in the continuous singular case
  /// @param capacity not applicable to class
  /// @param config not applicable to class
  /// @return true always
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) override;

 protected:
  // Stores summary statistics for all inserted aggregate chunks into this queue structure
  Aggregate running_aggregate_{};
};

}  // namespace statistics
}  // namespace esphome
