/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"

#include <vector>

namespace esphome {
namespace statistics {

class RunningQueue {
 public:
  // Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
  //  - returns whether memory was successfully allocated
  bool set_capacity(size_t capacity);

  // Clears all readings
  void clear();

  // Insert a value at end
  void insert(float value);

  Aggregate compute_current_aggregate();

 protected:
  Aggregate *queue_{nullptr};
  uint8_t index_{0};

  inline Aggregate get_end_();
};

}  // namespace statistics
}  // namespace esphome
