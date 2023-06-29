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
bool RunningQueue::set_capacity(const size_t capacity, const EnabledAggregatesConfiguration config) {
  this->config_ = config;

  uint8_t aggregate_capacity = 32;
  if (capacity > 0)
    aggregate_capacity = std::ceil(std::log2(capacity)) + 1;

  if (!this->queue_.set_capacity(aggregate_capacity, this->config_))
    return false;

  return true;
}

void RunningQueue::clear() { this->index_ = 0; }

// Insert value
void RunningQueue::insert(const float value) {
  Aggregate new_aggregate = Aggregate(value);

  Aggregate most_recent = this->get_end_();

  while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
    --this->index_;
    new_aggregate = most_recent + new_aggregate;
    most_recent = this->get_end_();
  }

  this->queue_.emplace(new_aggregate, this->index_);
  ++this->index_;
}

Aggregate RunningQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total + this->queue_.lower(i);
  }

  return total;
}

inline Aggregate RunningQueue::get_end_() {
  return (this->index_ == 0) ? Aggregate() : this->queue_.lower(this->index_ - 1);
}

}  // namespace statistics
}  // namespace esphome
