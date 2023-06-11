/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2 (currently uses n+3, can probably fix this by handling the e_ index better)
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)

  Implemented by Kevin Ahrendt, June 2023
*/

#include "daba_lite.h"
#include "circular_queue_index.h"
#include "aggregate.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <vector>

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

// set capacity (and reserve in memory) of the circular queues for the desired statistics
void DABALite::set_capacity(size_t window_size) {
  // DABA Lite requires an index to point to one entry past the end of the queue, hence +1
  this->window_size_ = window_size + 1;

  if (this->include_max_)
    this->max_queue_.reserve(this->window_size_);
  if (this->include_min_)
    this->min_queue_.reserve(this->window_size_);
  if (this->include_count_)
    this->count_queue_.reserve(this->window_size_);
  if (this->include_mean_)
    this->mean_queue_.reserve(this->window_size_);
  if (this->include_m2_)
    this->m2_queue_.reserve(this->window_size_);
  if (this->include_c2_)
    this->c2_queue_.reserve(this->window_size_);
  if (this->include_t_m2_)
    this->t_m2_queue_.reserve(this->window_size_);
  if (this->include_t_mean_)
    this->t_mean_queue_.reserve(this->window_size_);

  this->f_ = CircularQueueIndex(0, this->window_size_);
  this->l_ = CircularQueueIndex(0, this->window_size_);
  this->r_ = CircularQueueIndex(0, this->window_size_);
  this->a_ = CircularQueueIndex(0, this->window_size_);
  this->b_ = CircularQueueIndex(0, this->window_size_);
  this->e_ = CircularQueueIndex(0, this->window_size_);
}

// number of measurements currently in the windowQ
size_t DABALite::size() const {
  if (this->e_ == this->f_)
    return 0;
  if (this->e_.get_index() < this->f_.get_index())
    return (this->e_.get_index() + this->e_.get_capacity() - this->f_.get_index());

  return (this->e_.get_index() - this->f_.get_index());
}

// insert value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  Aggregate lifted = this->lift_(value);
  this->back_sum_ = this->combine_(this->back_sum_, lifted);

  this->emplace_(lifted, this->e_.get_index());

  ++this->e_;
  this->step_();

  this->is_current_aggregate_updated_ = false;
}

// remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  ++this->f_;

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

// compute summary statistics for a single measurement and returns them as an Aggregate
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
    if (this->include_t_m2_)
      part.set_t_m2(0.0);
    if (this->include_t_mean_)
      part.set_t_mean(static_cast<double>(now) / 1000.0);
  }

  return part;
}

// store an Aggregate at an index only in the enabled queue_ vectors
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
  if (this->include_t_mean_)
    this->t_mean_queue_[index] = value.get_t_mean();
  if (this->include_t_m2_)
    this->t_m2_queue_[index] = value.get_t_m2();
}

// combine summary statistics from two Aggregates
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
  if (this->include_t_mean_)
    part.combine_t_mean(a, b);
  if (this->include_t_m2_)
    part.combine_t_m2(a, b);

  return part;
}

// return summary statistics at given index as an Aggregate
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
  if (this->include_t_m2_)
    aggregate.set_t_m2(this->t_m2_queue_[index]);
  if (this->include_t_mean_)
    aggregate.set_t_mean(this->t_mean_queue_[index]);

  return aggregate;
}

// DABA Lite algorithm method
void DABALite::step_() {
  // this->debug_pointers_();
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->f_ != this->b_) {
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

void DABALite::debug_pointers_() {
  int head = this->f_.get_index();
  int tail = this->e_.get_index();
  int capacity = this->window_size_;

  size_t f = 0;
  size_t l = (((static_cast<int>(this->l_.get_index()) - head) % capacity) + capacity) % capacity;
  size_t r = (((static_cast<int>(this->r_.get_index()) - head) % capacity) + capacity) % capacity;
  size_t a = (((static_cast<int>(this->a_.get_index()) - head) % capacity) + capacity) % capacity;
  size_t b = (((static_cast<int>(this->b_.get_index()) - head) % capacity) + capacity) % capacity;
  size_t e = (((tail - head) % capacity) + capacity) % capacity;

  ESP_LOGI("iterates (raw)", "f=%d; l=%d; r=%d, a=%d, b=%d, e=%d", head, this->l_.get_index(), this->r_.get_index(),
           this->a_.get_index(), this->b_.get_index(), tail);
  ESP_LOGI("iterates (shifted)", "f=%d; l=%d; r=%d, a=%d, b=%d; e=%d", f, l, r, a, b, e);
  if ((l > r) || (r > a) || (a > b)) {
    ESP_LOGE("daba", "pointers in wrong order!");
  }
  ESP_LOGI("iterates", "queue_size=%d", this->size());
}

// DABA Lite algorithm methods
inline bool DABALite::is_front_empty_() { return this->b_ == this->f_; }
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
