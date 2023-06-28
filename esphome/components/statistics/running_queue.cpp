/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "running_queue.h"

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

#include <cmath>
#include <vector>

namespace esphome {
namespace statistics {

// Set capacity (and reserve in memory) of the circular queues for the desired statistics
//  - returns whether memory was successfully allocated
bool RunningQueue::set_capacity(size_t reset_after) {
  uint8_t aggregate_capacity = 32;
  if (reset_after > 0)
    aggregate_capacity = std::ceil(std::log2(reset_after)) + 1;

  // Mimics ESPHome's rp2040_pio_led_strip component's buf_ code (accessed June 2023)
  ExternalRAMAllocator<Aggregate> aggregate_allocator(ExternalRAMAllocator<Aggregate>::ALLOW_FAILURE);

  this->queue_ = aggregate_allocator.allocate(aggregate_capacity);

  if (this->queue_ == nullptr)
    return false;

  this->queue_[index_] = Aggregate();

  return true;
}

void RunningQueue::clear() {
  this->queue_[0] = Aggregate();
  this->index_ = 0;
}

// Insert value
void RunningQueue::insert(float value) {
  Aggregate new_aggregate = Aggregate(value);

  if (this->index_ == 0) {
    this->queue_[0] = new_aggregate;
    ++this->index_;
  } else {
    Aggregate most_recent = this->queue_[this->index_ - 1];

    while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
      --this->index_;
      new_aggregate = most_recent + new_aggregate;
      if (this->index_ == 0)
        most_recent = Aggregate();
      else
        most_recent = this->queue_[this->index_ - 1];
    }

    this->queue_[this->index_] = new_aggregate;
    ++this->index_;
  }
}

Aggregate RunningQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total + this->queue_[i];
  }

  return total;
}

}  // namespace statistics
}  // namespace esphome
