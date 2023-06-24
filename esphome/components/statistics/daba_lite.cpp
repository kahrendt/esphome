/*
 * Sliding window aggregates are stored and computed using the De-Amortized Banker's Aggregator Lite (DABA Lite)
 * algorithm
 *  - space requirements: n+2 aggregates
 *  - time complexity: worse-case O(1)
 *  - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "aggregate.h"
#include "circular_queue_index.h"
#include "daba_lite.h"

#include "esphome/core/hal.h"      // necessary for millis()
#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

namespace esphome {
namespace statistics {

float DABALite::aggregated_count() {
  this->update_current_aggregate_();
  return this->current_aggregate_.get_count();
}

float DABALite::aggregated_max() {
  this->update_current_aggregate_();
  return this->current_aggregate_.get_max();
}

float DABALite::aggregated_min() {
  this->update_current_aggregate_();
  return this->current_aggregate_.get_min();
}

float DABALite::aggregated_mean() {
  this->update_current_aggregate_();
  return this->current_aggregate_.get_mean();
}

float DABALite::aggregated_variance() {
  this->update_current_aggregate_();
  return this->current_aggregate_.compute_variance();
}

float DABALite::aggregated_std_dev() {
  this->update_current_aggregate_();
  return this->current_aggregate_.compute_std_dev();
}

float DABALite::aggregated_covariance() {
  this->update_current_aggregate_();
  return this->current_aggregate_.compute_covariance();
}
float DABALite::aggregated_trend() {
  this->update_current_aggregate_();
  return this->current_aggregate_.compute_trend();
}

// Set capacity (and reserve in memory) of the circular queues for the desired statistics
//  - returns whether memory was successfully allocated
bool DABALite::set_capacity(size_t window_size) {
  this->window_size_ = window_size;

  // Mimics ESPHome's rp2040_pio_led_strip component's buf_ code (accessed June 2023)
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  ExternalRAMAllocator<size_t> size_t_allocator(ExternalRAMAllocator<size_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<int32_t> int32_t_allocator(ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<uint32_t> uint32_t_allocator(ExternalRAMAllocator<uint32_t>::ALLOW_FAILURE);

  if (this->include_max_) {
    this->max_queue_ = float_allocator.allocate(this->window_size_);
    if (this->max_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_min_) {
    this->min_queue_ = float_allocator.allocate(this->window_size_);
    if (this->min_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_count_) {
    this->count_queue_ = size_t_allocator.allocate(this->window_size_);
    if (this->count_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_mean_) {
    this->mean_queue_ = float_allocator.allocate(this->window_size_);
    if (this->mean_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_m2_) {
    this->m2_queue_ = float_allocator.allocate(this->window_size_);
    if (this->m2_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_c2_) {
    this->c2_queue_ = float_allocator.allocate(this->window_size_);
    if (this->c2_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_timestamp_m2_) {
    this->timestamp_m2_queue_ = float_allocator.allocate(this->window_size_);
    if (this->timestamp_m2_queue_ == nullptr) {
      return false;
    }
  }

  if (this->include_timestamp_mean_) {
    this->timestamp_sum_queue_ = int32_t_allocator.allocate(this->window_size_);
    if (this->timestamp_sum_queue_ == nullptr) {
      return false;
    }

    this->timestamp_reference_queue_ = uint32_t_allocator.allocate(this->window_size_);
    if (this->timestamp_reference_queue_ == nullptr) {
      return false;
    }
  }

  this->clear();

  return true;
}

void DABALite::clear() {
  this->size_ = 0;  // set size of valid readings to 0

  // Reset Indices in the circular queue to the start
  this->f_ = CircularQueueIndex(0, this->window_size_);
  this->l_ = CircularQueueIndex(0, this->window_size_);
  this->r_ = CircularQueueIndex(0, this->window_size_);
  this->a_ = CircularQueueIndex(0, this->window_size_);
  this->b_ = CircularQueueIndex(0, this->window_size_);
  this->e_ = CircularQueueIndex(0, this->window_size_);
}

// Insert value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  Aggregate lifted = this->lift_(value);
  this->back_sum_ = this->combine_(this->back_sum_, lifted);

  this->emplace_(lifted, this->e_.get_index());

  ++this->e_;
  ++this->size_;
  this->step_();

  this->is_current_aggregate_updated_ = false;
}

// Remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  ++this->f_;
  --this->size_;

  this->step_();

  this->is_current_aggregate_updated_ = false;
}

// Update current_aggregate_ to account for latest changes
void DABALite::update_current_aggregate_() {
  if (!this->is_current_aggregate_updated_) {
    if (this->size() > 0) {
      Aggregate alpha = this->get_alpha_();
      Aggregate back = this->get_back_();

      this->current_aggregate_ = this->combine_(alpha, back);
    } else {
      this->current_aggregate_ = this->identity_class_;
    }
  }
  this->is_current_aggregate_updated_ = true;
}

// Compute aggregates for a single measurement v and return it as an Aggregate
Aggregate DABALite::lift_(float v) {
  const uint32_t now = millis();

  Aggregate part = this->identity_class_;

  if (!std::isnan(v)) {
    if (this->include_max_)
      part.set_max(v);
    if (this->include_min_)
      part.set_min(v);
    if (this->include_count_)
      part.set_count(1);
    if (this->include_mean_)
      part.set_mean(v);
    if (this->include_m2_)
      part.set_m2(0.0);
    if (this->include_c2_)
      part.set_c2(0.0);
    if (this->include_timestamp_m2_)
      part.set_timestamp_m2(0.0);
    if (this->include_timestamp_mean_) {
      part.set_timestamp_sum(0);
      part.set_timestamp_reference(now);
    }
  }

  return part;
}

// Store an Aggregate at specified index only in the enabled queues
void DABALite::emplace_(const Aggregate &value, size_t index) {
  if (this->include_max_)
    this->max_queue_[index] = value.get_max();
  if (this->include_min_)
    this->min_queue_[index] = value.get_min();
  if (this->include_count_)
    this->count_queue_[index] = value.get_count();
  if (this->include_mean_)
    this->mean_queue_[index] = value.get_mean();
  if (this->include_m2_)
    this->m2_queue_[index] = value.get_m2();
  if (this->include_c2_)
    this->c2_queue_[index] = value.get_c2();
  if (this->include_timestamp_mean_) {
    this->timestamp_sum_queue_[index] = value.get_timestamp_sum();
    this->timestamp_reference_queue_[index] = value.get_timestamp_reference();
  }
  if (this->include_timestamp_m2_)
    this->timestamp_m2_queue_[index] = value.get_timestamp_m2();
}

// Combine Aggregates for two disjoint sets of measurements
Aggregate DABALite::combine_(const Aggregate &a, const Aggregate &b) {
  Aggregate part;

  if (this->include_max_)
    part.combine_max(a, b);
  if (this->include_min_)
    part.combine_min(a, b);
  if (this->include_count_)
    part.combine_count(a, b);
  if (this->include_mean_)
    part.combine_mean(a, b);
  if (this->include_m2_)
    part.combine_m2(a, b);
  if (this->include_c2_)
    part.combine_c2(a, b);
  if (this->include_timestamp_mean_) {
    part.combine_timestamp_sum(a, b);
  }

  if (this->include_timestamp_m2_)
    part.combine_timestamp_m2(a, b);

  return part;
}

// Return aggregates at a given index in the queues
Aggregate DABALite::lower_(size_t index) {
  Aggregate aggregate = this->identity_class_;

  if (this->include_max_)
    aggregate.set_max(this->max_queue_[index]);
  if (this->include_min_)
    aggregate.set_min(this->min_queue_[index]);
  if (this->include_count_)
    aggregate.set_count(this->count_queue_[index]);
  if (this->include_mean_)
    aggregate.set_mean(this->mean_queue_[index]);
  if (this->include_m2_)
    aggregate.set_m2(this->m2_queue_[index]);
  if (this->include_c2_)
    aggregate.set_c2(this->c2_queue_[index]);
  if (this->include_timestamp_m2_)
    aggregate.set_timestamp_m2(this->timestamp_m2_queue_[index]);
  if (this->include_timestamp_mean_) {
    aggregate.set_timestamp_sum(this->timestamp_sum_queue_[index]);
    aggregate.set_timestamp_reference(this->timestamp_reference_queue_[index]);
  }

  return aggregate;
}

// DABA Lite algorithm method
void DABALite::step_() {
  // this->debug_pointers_();
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->size_ > 0) {
    if (this->a_ != this->r_) {
      Aggregate prev_delta = this->get_delta_();

      --this->a_;
      Aggregate old_a = this->lower_(this->a_.get_index());

      this->emplace_(combine_(old_a, prev_delta), this->a_.get_index());
    }

    if (this->l_ != this->r_) {
      Aggregate old_l = this->lower_(this->l_.get_index());

      this->emplace_(combine_(old_l, this->mid_sum_), this->l_.get_index());
      ++this->l_;
    } else {
      ++this->l_;
      ++this->r_;
      ++this->a_;
      this->mid_sum_ = get_delta_();
    }
  } else {
    this->back_sum_ = this->mid_sum_ = this->identity_class_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->f_;
  this->r_ = this->b_;
  this->a_ = this->e_;
  this->b_ = this->e_;

  this->mid_sum_ = this->back_sum_;
  this->back_sum_ = this->identity_class_;
}

// DABA Lite algorithm methods

// Checks if the b_ index is equal to the front index f_;
//  - Note if window size == size of queue, then the front and end indices point to the same index,
//    so we verify that this is not the case
inline bool DABALite::is_front_empty_() { return (this->b_ == this->f_) && (this->size_ != this->window_size_); }

inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
inline Aggregate DABALite::get_back_() { return this->back_sum_; }
inline Aggregate DABALite::get_alpha_() {
  return this->is_front_empty_() ? this->identity_class_ : this->lower_(this->f_.get_index());
}
inline Aggregate DABALite::get_delta_() {
  return this->is_delta_empty_() ? this->identity_class_ : this->lower_(this->a_.get_index());
}
}  // namespace statistics
}  // namespace esphome
