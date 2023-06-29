/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"

#include <vector>

namespace esphome {
namespace statistics {

class RunningQueue : AggregateQueue<float> {
 public:
  // Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config);

  // Clears all aggregates in the queue
  void clear();

  // Insert a value at end of the queue and consolidiate if necessary
  void insert(float value);

  // Computes the summary statistics for all measurements stored in the queue
  Aggregate compute_current_aggregate();

 protected:
  uint8_t index_{0};

  inline Aggregate get_end_();
};

}  // namespace statistics
}  // namespace esphome
