/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

namespace esphome {
namespace statistics {

class RunningSingular : public AggregateQueue {
 public:
  // no need to set capacity in the singular running case
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) override { return true; }

  // Clears all aggregates in the queue
  void clear() override;
  void evict() override { this->clear(); }

  // Insert a value at end of the queue and consolidiate if necessary
  void insert(float value, uint32_t duration) override;
  ;
  void insert(Aggregate value) override;

  // Computes the summary statistics for all aggregated measurements
  Aggregate compute_current_aggregate() override { return this->running_aggregate_; }

 protected:
  Aggregate running_aggregate_{};
};

}  // namespace statistics
}  // namespace esphome
