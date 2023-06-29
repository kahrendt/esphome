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

// Sets the capacity of underlying queue; uses at most log_2(n)+1 aggregates
//  - returns whether memory was successfully allocated
template<typename T> bool RunningQueue<T>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config) {
  uint8_t aggregate_capacity = 32;
  if (capacity > 0)
    aggregate_capacity = std::ceil(std::log2(capacity)) + 1;

  if (!this->allocate_memory(aggregate_capacity, config))
    return false;

  this->clear();

  return true;
}

// Clears all aggregates in the queue
template<typename T> void RunningQueue<T>::clear() { this->index_ = 0; }

// Insert a value at end of the queue and consolidiate if necessary
template<typename T> void RunningQueue<T>::insert(T value, uint32_t time_delta) {
  Aggregate new_aggregate = Aggregate(value, time_delta);

  Aggregate most_recent = this->get_end_();

  while ((this->index_ > 0) && (most_recent.get_count() <= new_aggregate.get_count())) {
    --this->index_;
    new_aggregate = most_recent + new_aggregate;
    most_recent = this->get_end_();
  }

  this->emplace(new_aggregate, this->index_);
  ++this->index_;
}

// Computes the summary statistics for all measurements stored in the queue
template<typename T> Aggregate RunningQueue<T>::compute_current_aggregate() {
  Aggregate total = Aggregate();
  for (int i = this->index_ - 1; i >= 0; --i) {
    total = total + this->lower(i);
  }

  return total;
}

template<typename T> inline Aggregate RunningQueue<T>::get_end_() {
  return (this->index_ == 0) ? Aggregate() : this->lower(this->index_ - 1);
}

// avoids linking errors (https://isocpp.org/wiki/faq/templates)
template bool RunningQueue<float>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void RunningQueue<float>::clear();
template void RunningQueue<float>::insert(float value, uint32_t time_delta);
template Aggregate RunningQueue<float>::compute_current_aggregate();

template bool RunningQueue<double>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void RunningQueue<double>::clear();
template void RunningQueue<double>::insert(double value, uint32_t time_delta);
template Aggregate RunningQueue<double>::compute_current_aggregate();

}  // namespace statistics
}  // namespace esphome
