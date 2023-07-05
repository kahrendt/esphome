/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "aggregate_queue.h"
#include "running_queue.h"

namespace esphome {
namespace statistics {

// Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
//  - returns whether memory was successfully allocated
bool RunningQueue::set_capacity(size_t capacity, EnabledAggregatesConfiguration config) {
  uint8_t aggregate_capacity = 32;
  if (capacity > 0)
    aggregate_capacity = std::ceil(std::log2(capacity)) + 1;

  if (!this->allocate_memory(aggregate_capacity, config))
    return false;

  this->clear();

  return true;
}

// Clears all aggregates in the queue
void RunningQueue::clear() {
  this->index_ = 0;
  this->size_ = 0;
}

// Insert a value at end of the queue and consolidiate if necessary
void RunningQueue::insert(Aggregate new_aggregate) {
  Aggregate most_recent = this->get_end_();

  while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
    --this->index_;
    new_aggregate = new_aggregate.combine_with(most_recent, this->time_weighted_);  // most_recent + new_aggregate;
    most_recent = this->get_end_();
  }

  this->emplace(new_aggregate, this->index_);
  ++this->index_;
  ++this->size_;
}

// Computes the summary statistics for all measurements stored in the queue
Aggregate RunningQueue::compute_current_aggregate() {
  Aggregate total = Aggregate();
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total.combine_with(this->lower(i), this->time_weighted_);  // total + this->lower(i);
  }

  return total;
}

inline Aggregate RunningQueue::get_end_() { return (this->index_ == 0) ? Aggregate() : this->lower(this->index_ - 1); }

}  // namespace statistics
}  // namespace esphome
