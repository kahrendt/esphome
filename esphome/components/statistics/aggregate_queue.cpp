/*

 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "aggregate_queue.h"

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

namespace esphome {
namespace statistics {

template<typename T> void AggregateQueue<T>::emplace(const Aggregate &value, size_t index) {
  if (this->max_queue_ != nullptr)
    this->max_queue_[index] = value.get_max();
  if (this->min_queue_ != nullptr)
    this->min_queue_[index] = value.get_min();
  if (this->count_queue_ != nullptr)
    this->count_queue_[index] = value.get_count();
  if (this->duration_queue_ != nullptr)
    this->duration_queue_[index] = value.get_duration();
  if (this->duration_squared_queue_ != nullptr)
    this->duration_squared_queue_[index] = value.get_duration_squared();
  if (this->mean_queue_ != nullptr) {
    this->mean_queue_[index] = value.get_mean();

    this->mean2_queue_[index] = value.get_mean2();
    this->mean3_queue_[index] = value.get_mean3();
    this->mean4_queue_[index] = value.get_mean4();
  }
  if (this->m2_queue_ != nullptr)
    this->m2_queue_[index] = value.get_m2();
  if (this->c2_queue_ != nullptr)
    this->c2_queue_[index] = value.get_c2();
  if (this->timestamp_mean_queue_ != nullptr)
    this->timestamp_mean_queue_[index] = value.get_timestamp_mean();
  if (this->timestamp_m2_queue_ != nullptr)
    this->timestamp_m2_queue_[index] = value.get_timestamp_m2();
  if (this->timestamp_reference_queue_ != nullptr)
    this->timestamp_reference_queue_[index] = value.get_timestamp_reference();
}

template<typename T> Aggregate AggregateQueue<T>::lower(size_t index) {
  Aggregate aggregate = Aggregate();

  if (this->max_queue_ != nullptr)
    aggregate.set_max(this->max_queue_[index]);
  if (this->min_queue_ != nullptr)
    aggregate.set_min(this->min_queue_[index]);
  if (this->count_queue_ != nullptr)
    aggregate.set_count(this->count_queue_[index]);
  if (this->duration_queue_ != nullptr)
    aggregate.set_duration(this->duration_queue_[index]);
  if (this->duration_squared_queue_ != nullptr)
    aggregate.set_duration_squared(this->duration_squared_queue_[index]);
  if (this->mean_queue_ != nullptr) {
    aggregate.set_mean(this->mean_queue_[index]);

    aggregate.set_mean2(this->mean2_queue_[index]);
    aggregate.set_mean3(this->mean3_queue_[index]);
    aggregate.set_mean4(this->mean4_queue_[index]);
  }
  if (this->m2_queue_ != nullptr)
    aggregate.set_m2(this->m2_queue_[index]);
  if (this->c2_queue_ != nullptr)
    aggregate.set_c2(this->c2_queue_[index]);
  if (this->timestamp_m2_queue_ != nullptr)
    aggregate.set_timestamp_m2(this->timestamp_m2_queue_[index]);
  if (this->timestamp_mean_queue_ != nullptr)
    aggregate.set_timestamp_mean(this->timestamp_mean_queue_[index]);
  if (this->timestamp_reference_queue_ != nullptr)
    aggregate.set_timestamp_reference(this->timestamp_reference_queue_[index]);

  return aggregate;
}

template<typename T> bool AggregateQueue<T>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config) {
  // Mimics ESPHome's rp2040_pio_led_strip component's buf_ code (accessed June 2023)
  ExternalRAMAllocator<T> decimal_allocator(ExternalRAMAllocator<T>::ALLOW_FAILURE);
  ExternalRAMAllocator<size_t> size_t_allocator(ExternalRAMAllocator<size_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<uint32_t> uint32_t_allocator(ExternalRAMAllocator<uint32_t>::ALLOW_FAILURE);

  if (config.max) {
    this->max_queue_ = decimal_allocator.allocate(capacity);
    if (this->max_queue_ == nullptr) {
      return false;
    }
  }

  if (config.min) {
    this->min_queue_ = decimal_allocator.allocate(capacity);
    if (this->min_queue_ == nullptr) {
      return false;
    }
  }

  if (config.count) {
    this->count_queue_ = size_t_allocator.allocate(capacity);
    if (this->count_queue_ == nullptr) {
      return false;
    }
  }

  if (config.duration) {
    this->duration_queue_ = uint32_t_allocator.allocate(capacity);  // uint32_t_allocator.allocate(capacity);
    if (this->duration_queue_ == nullptr) {
      return false;
    }
  }

  if (config.duration_squared) {
    this->duration_squared_queue_ = size_t_allocator.allocate(capacity);

    if (this->duration_squared_queue_ == nullptr) {
      return false;
    }
  }

  if (config.mean) {
    this->mean_queue_ = decimal_allocator.allocate(capacity);

    this->mean2_queue_ = decimal_allocator.allocate(capacity);
    this->mean3_queue_ = decimal_allocator.allocate(capacity);
    this->mean4_queue_ = decimal_allocator.allocate(capacity);

    if (this->mean_queue_ == nullptr) {
      return false;
    }
  }

  if (config.m2) {
    this->m2_queue_ = decimal_allocator.allocate(capacity);
    if (this->m2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.c2) {
    this->c2_queue_ = decimal_allocator.allocate(capacity);
    if (this->c2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_m2) {
    this->timestamp_m2_queue_ = decimal_allocator.allocate(capacity);
    if (this->timestamp_m2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_mean) {
    this->timestamp_mean_queue_ = decimal_allocator.allocate(capacity);
    if (this->timestamp_mean_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_reference) {
    this->timestamp_reference_queue_ = uint32_t_allocator.allocate(capacity);
    if (this->timestamp_reference_queue_ == nullptr) {
      return false;
    }
  }

  return true;
}

// avoids linking errors (https://isocpp.org/wiki/faq/templates)

template void AggregateQueue<float>::emplace(const Aggregate &value, size_t index);
template Aggregate AggregateQueue<float>::lower(size_t index);
template bool AggregateQueue<float>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

template void AggregateQueue<double>::emplace(const Aggregate &value, size_t index);
template Aggregate AggregateQueue<double>::lower(size_t index);
template bool AggregateQueue<double>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

}  // namespace statistics
}  // namespace esphome
