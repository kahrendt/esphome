/*

 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "aggregate_queue.h"

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

namespace esphome {
namespace statistics {

void AggregateQueue::emplace(const Aggregate &value, size_t index) {
  if (this->c2_queue_ != nullptr)
    this->c2_queue_[index] = value.get_c2();

  if (this->count_queue_ != nullptr)
    this->count_queue_[index] = value.get_count();

  if (this->duration_queue_ != nullptr)
    this->duration_queue_[index] = value.get_duration();
  if (this->duration_squared_queue_ != nullptr)
    this->duration_squared_queue_[index] = value.get_duration_squared();

  if (this->m2_queue_ != nullptr)
    this->m2_queue_[index] = value.get_m2();

  if (this->max_queue_ != nullptr)
    this->max_queue_[index] = value.get_max();

  if (this->mean_queue_ != nullptr)
    this->mean_queue_[index] = value.get_mean();

  if (this->min_queue_ != nullptr)
    this->min_queue_[index] = value.get_min();

  if (this->timestamp_m2_queue_ != nullptr)
    this->timestamp_m2_queue_[index] = value.get_timestamp_m2();
  if (this->timestamp_mean_queue_ != nullptr)
    this->timestamp_mean_queue_[index] = value.get_timestamp_mean();
  if (this->timestamp_reference_queue_ != nullptr)
    this->timestamp_reference_queue_[index] = value.get_timestamp_reference();
}

Aggregate AggregateQueue::lower(size_t index) {
  Aggregate aggregate = Aggregate();

  if (this->c2_queue_ != nullptr)
    aggregate.set_c2(this->c2_queue_[index]);

  if (this->count_queue_ != nullptr)
    aggregate.set_count(this->count_queue_[index]);

  if (this->duration_queue_ != nullptr)
    aggregate.set_duration(this->duration_queue_[index]);
  if (this->duration_squared_queue_ != nullptr)
    aggregate.set_duration_squared(this->duration_squared_queue_[index]);

  if (this->m2_queue_ != nullptr)
    aggregate.set_m2(this->m2_queue_[index]);

  if (this->max_queue_ != nullptr)
    aggregate.set_max(this->max_queue_[index]);

  if (this->mean_queue_ != nullptr)
    aggregate.set_mean(this->mean_queue_[index]);

  if (this->min_queue_ != nullptr)
    aggregate.set_min(this->min_queue_[index]);

  if (this->timestamp_m2_queue_ != nullptr)
    aggregate.set_timestamp_m2(this->timestamp_m2_queue_[index]);
  if (this->timestamp_mean_queue_ != nullptr)
    aggregate.set_timestamp_mean(this->timestamp_mean_queue_[index]);
  if (this->timestamp_reference_queue_ != nullptr)
    aggregate.set_timestamp_reference(this->timestamp_reference_queue_[index]);

  return aggregate;
}

bool AggregateQueue::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config) {
  // Mimics ESPHome's rp2040_pio_led_strip component's buf_ code (accessed June 2023)
  ExternalRAMAllocator<double> double_allocator(ExternalRAMAllocator<double>::ALLOW_FAILURE);
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  ExternalRAMAllocator<size_t> size_t_allocator(ExternalRAMAllocator<size_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<uint32_t> uint32_t_allocator(ExternalRAMAllocator<uint32_t>::ALLOW_FAILURE);

  if (config.c2) {
    this->c2_queue_ = double_allocator.allocate(capacity);
    if (this->c2_queue_ == nullptr)
      return false;
  }

  if (config.count) {
    this->count_queue_ = size_t_allocator.allocate(capacity);
    if (this->count_queue_ == nullptr)
      return false;
  }

  if (config.duration) {
    this->duration_queue_ = size_t_allocator.allocate(capacity);  // uint32_t_allocator.allocate(capacity);
    if (this->duration_queue_ == nullptr)
      return false;
  }

  if (config.duration_squared) {
    this->duration_squared_queue_ = size_t_allocator.allocate(capacity);

    if (this->duration_squared_queue_ == nullptr)
      return false;
  }

  if (config.m2) {
    this->m2_queue_ = double_allocator.allocate(capacity);
    if (this->m2_queue_ == nullptr)
      return false;
  }

  if (config.max) {
    this->max_queue_ = float_allocator.allocate(capacity);
    if (this->max_queue_ == nullptr)
      return false;
  }

  if (config.mean) {
    this->mean_queue_ = float_allocator.allocate(capacity);

    if (this->mean_queue_ == nullptr)
      return false;
  }

  if (config.min) {
    this->min_queue_ = float_allocator.allocate(capacity);
    if (this->min_queue_ == nullptr)
      return false;
  }

  if (config.timestamp_m2) {
    this->timestamp_m2_queue_ = double_allocator.allocate(capacity);
    if (this->timestamp_m2_queue_ == nullptr)
      return false;
  }

  if (config.timestamp_mean) {
    this->timestamp_mean_queue_ = double_allocator.allocate(capacity);
    if (this->timestamp_mean_queue_ == nullptr)
      return false;
  }

  if (config.timestamp_reference) {
    this->timestamp_reference_queue_ = uint32_t_allocator.allocate(capacity);
    if (this->timestamp_reference_queue_ == nullptr)
      return false;
  }

  return true;
}

}  // namespace statistics
}  // namespace esphome
