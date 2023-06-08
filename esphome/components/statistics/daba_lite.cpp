#include "daba_lite.h"
#include "circular_queue.h"
#include "circular_queue.cpp"
#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace statistics {

void DABALite::set_identity() {
  if (this->include_max_)
    this->identity_class_.set_max();
  if (this->include_min_)
    this->identity_class_.set_min();
  if (this->include_count_)
    this->identity_class_.set_count();
  if (this->include_mean_)
    this->identity_class_.set_mean();
  if (this->include_m2_)
    this->identity_class_.set_m2();
  if (this->include_c2_) {
    this->identity_class_.set_c2();
    this->identity_class_.set_t_mean();
  }
  if (this->include_t_m2_) {
    this->identity_class_.set_t_m2();
  }

  this->midSum_ = this->identity_class_;
  this->backSum_ = this->identity_class_;
}

void DABALite::set_capacity(size_t window_size) {
  // DABA Lite requires an index to point to one entry past the end of the queue, hence +1
  this->window_size_ = window_size + 1;

  this->set_identity();

  this->queue_.reserve(this->window_size_);

  this->f_ = CircularQueueIndex(0, this->window_size_);
  this->l_ = CircularQueueIndex(0, this->window_size_);
  this->r_ = CircularQueueIndex(0, this->window_size_);
  this->a_ = CircularQueueIndex(0, this->window_size_);
  this->b_ = CircularQueueIndex(0, this->window_size_);
  this->e_ = CircularQueueIndex(0, this->window_size_);
}

size_t DABALite::size() {
  if (this->e_ == this->f_)
    return 0;
  if (this->e_.get_index() < this->f_.get_index())
    return (this->e_.get_index() + this->e_.get_capacity() - this->f_.get_index());

  return (this->e_.get_index() - this->f_.get_index());
}

// insert a new value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  AggregateClass lifted = this->lift_(value);
  this->backSum_ = this->combine_(this->backSum_, lifted);
  this->queue_[this->e_.get_index()] = lifted;
  ++this->e_;
  this->step_();
}

// remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  ++this->f_;

  this->step_();
}

// Return the summary statistics for all entries in the queue
AggregateClass DABALite::query() {
  if (this->size() > 0) {
    AggregateClass alpha = this->get_alpha_();
    AggregateClass back = this->get_back_();

    return this->combine_(alpha, back);
  } else
    return this->identity_class_;
}

AggregateClass DABALite::lift_(float v) {
  const uint32_t now = millis();

  AggregateClass part = this->identity_class_;

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
    if (this->include_t_m2_) {
      part.set_t_m2(0.0);
      part.set_t_mean(static_cast<double>(now) / 1000.0);
    }
  }

  return part;
}
// Aggregate DABALite::lift_(float v) {
//   const uint32_t now = millis();

//   Aggregate part = this->identity_;

//   if (!std::isnan(v)) {
//     part.max = v;
//     part.min = v;

//     part.count = 1;

//     part.m2 = 0.0;

//     part.mean = v;

//     part.t_mean = static_cast<double>(now) / 1000.0;  // divide by 1000 to convert into seconds
//     part.t_m2 = 0.0;
//     part.c2 = 0.0;
//   }

//   return part;
// }

// combine two aggregates summary statistics
AggregateClass DABALite::combine_(AggregateClass &a, AggregateClass &b) {
  AggregateClass part;

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
  if (this->include_c2_) {
    part.combine_t_mean(a, b);
    part.combine_c2(a, b);
  }

  if (this->include_t_m2_) {
    part.combine_t_m2(a, b);
  }

  return part;
}
// Aggregate DABALite::combine_(Aggregate &a, Aggregate &b) {
//   Aggregate part;

//   part.max = std::max(a.max, b.max);
//   part.min = std::min(a.min, b.min);

//   part.count = a.count + b.count;

//   if (std::isnan(a.mean) && std::isnan(b.mean)) {
//     part.m2 = NAN;
//     part.mean = NAN;
//     part.t_mean = NAN;
//     part.c2 = NAN;
//     part.t_m2 = NAN;
//   } else if (std::isnan(a.mean)) {  // only a is NAN
//     part.m2 = b.m2;
//     part.mean = b.mean;
//     part.t_mean = b.t_mean;
//     part.t_m2 = b.t_m2;
//     part.c2 = b.c2;
//   } else if (std::isnan(b.mean)) {  // only b is NAN
//     part.m2 = a.m2;
//     part.mean = a.mean;
//     part.t_mean = a.t_mean;
//     part.t_m2 = a.t_m2;
//     part.c2 = a.c2;
//   } else {  // both valid
//     double a_count = static_cast<float>(a.count);
//     double b_count = static_cast<float>(b.count);
//     double part_count = static_cast<double>(part.count);

//     // weighted average of the two means based on their counts
//     // reduces the chances for floating point errors in comparison to saving the aggregate sum and dividing by the
//     part.mean = (a.mean * a_count + b.mean * b_count) / part_count;
//     part.t_mean = (a.t_mean * a_count + b.t_mean * b_count) / part_count;

//     // compute overall M2 for Welford's algorithm using Chan's parallel algorithm for computing the variance
//     // drastically reduces the chances for catastrophic cancellation with floating point arithmetic
//     float delta = b.mean - a.mean;
//     part.m2 = a.m2 + b.m2 + delta * delta * a_count * b_count / part_count;

//     // compute M2 for Welford's algorithm for x-values; i.e., timestamps
//     float t_delta = b.t_mean - a.t_mean;
//     part.t_m2 = a.t_m2 + b.t_m2 + t_delta * t_delta * a_count * b_count / part_count;

//     // compute C2 for an extension of Welford's algorithm to compute the covariance of t (timestamps) and y (sensor
//     // measurements)
//     part.c2 = a.c2 + b.c2 + t_delta * delta * a_count * b_count / part_count;
//   }

//   return part;
// }

// DABA Lite algorithm method
void DABALite::step_() {
  // this->debug_pointers_();
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->f_ != this->b_) {
    if (this->a_ != this->r_) {
      AggregateClass prev_delta = this->get_delta_();

      --this->a_;
      AggregateClass old_a = this->queue_[this->a_.get_index()];

      this->queue_[this->a_.get_index()] = combine_(old_a, prev_delta);
    }

    if (this->l_ != this->r_) {
      AggregateClass old_l = this->queue_[this->l_.get_index()];
      this->queue_[this->l_.get_index()] = combine_(old_l, this->midSum_);

      ++this->l_;
    } else {
      ++this->l_;
      ++this->r_;
      ++this->a_;
      this->midSum_ = get_delta_();
    }
  } else {
    this->backSum_ = this->midSum_ = this->identity_class_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->f_;
  this->r_ = this->b_;
  this->a_ = this->e_;
  this->b_ = this->e_;

  this->midSum_ = this->backSum_;
  this->backSum_ = this->identity_class_;
}

void DABALite::debug_pointers_() {
  int head = this->f_.get_index();
  int tail = this->e_.get_index();
  int capacity = this->window_size_;

  size_t f = (head - head);
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
inline AggregateClass DABALite::get_back_() { return this->backSum_; }
inline AggregateClass DABALite::get_alpha_() {
  return this->is_front_empty_() ? this->identity_class_ : this->queue_[this->f_.get_index()];
}
inline AggregateClass DABALite::get_delta_() {
  return this->is_delta_empty_() ? this->identity_class_ : this->queue_[this->a_.get_index()];
}
}  // namespace statistics
}  // namespace esphome
