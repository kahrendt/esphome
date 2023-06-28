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
  bool set_capacity(size_t window_size);

  // Clears all readings
  void clear();

  // Insert a value at end
  void insert(float value);

  Aggregate compute_current_aggregate();

 protected:
  Aggregate *queue_{nullptr};

  size_t index_{0};
};

}  // namespace statistics
}  // namespace esphome
