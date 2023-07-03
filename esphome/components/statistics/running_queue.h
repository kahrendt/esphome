/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"

#include "esphome/core/log.h"

#include <vector>

namespace esphome {
namespace statistics {

template<typename T> class RunningQueue : public AggregateQueue<T> {
 public:
  void enable_time_weighted() override { this->time_weighted_ = true; }
  // Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) override;

  // Clears all aggregates in the queue
  void clear() override;
  void evict() override { this->clear(); }

  // Insert a value at end of the queue and consolidiate if necessary
  void insert(T value, uint32_t duration) override;
  void insert(Aggregate value) override;

  // number of chunks stored in the queue
  size_t size() const override { return size_; }

  // Computes the summary statistics for all measurements stored in the queue
  Aggregate compute_current_aggregate() override;

 protected:
  uint8_t index_{0};

  size_t size_{0};

  bool time_weighted_{false};

  inline Aggregate get_end_();
};

}  // namespace statistics
}  // namespace esphome
