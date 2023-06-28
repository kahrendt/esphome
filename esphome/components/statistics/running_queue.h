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
  // Sets the window size by adjusting the capacity of the underlying circular queues
  //  - returns whether memory was successfully allocated
  bool set_capacity(uint8_t capacity);

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
