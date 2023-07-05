/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "aggregate_queue.h"
#include "running_singular.h"

namespace esphome {
namespace statistics {

// Clears all aggregates in the queue
template<typename T> void RunningSingular<T>::clear() {
  this->running_aggregate_ = Aggregate();
  this->size_ = 0;
};

// Insert a value at end of the queue and consolidiate if necessary
template<typename T> void RunningSingular<T>::insert(T value, uint32_t duration) {
  this->insert(Aggregate(value, duration));
}

template<typename T> void RunningSingular<T>::insert(Aggregate value) {
  this->running_aggregate_ = this->running_aggregate_.combine_with(value, this->time_weighted_);
  ++this->size_;
}

// avoids linking errors (https://isocpp.org/wiki/faq/templates)
template bool RunningSingular<float>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void RunningSingular<float>::clear();
template void RunningSingular<float>::evict();
template size_t RunningSingular<float>::size() const;
template void RunningSingular<float>::insert(float value, uint32_t duration);
template void RunningSingular<float>::insert(Aggregate value);
template Aggregate RunningSingular<float>::compute_current_aggregate();

template bool RunningSingular<double>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void RunningSingular<double>::clear();
template void RunningSingular<double>::evict();
template size_t RunningSingular<double>::size() const;
template void RunningSingular<double>::insert(double value, uint32_t duration);
template void RunningSingular<double>::insert(Aggregate value);
template Aggregate RunningSingular<double>::compute_current_aggregate();

}  // namespace statistics
}  // namespace esphome
