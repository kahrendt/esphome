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
bool RunningQueue::set_capacity(size_t capacity) {
  uint8_t aggregate_capacity = 32;
  if (capacity > 0)
    aggregate_capacity = std::ceil(std::log2(capacity)) + 1;

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

  Aggregate most_recent = this->get_end_();

  while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
    --this->index_;
    new_aggregate = most_recent + new_aggregate;
    most_recent = this->get_end_();
  }

  this->queue_[this->index_] = new_aggregate;
  ++this->index_;
}

Aggregate RunningQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total + this->queue_[i];
  }

  return total;
}

inline Aggregate RunningQueue::get_end_() { return (this->index_ == 0) ? Aggregate() : this->queue_[this->index_ - 1]; }

}  // namespace statistics
}  // namespace esphome
