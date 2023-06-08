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
  this->set_identity();
  this->queue_class_.set_capacity_and_fill(window_size + 1, this->identity_class_);
}
// void DABALite::set_capacity(size_t window_size) {
//   // DABA Lite requires an index to point to one entry past the end of the queue, hence +1
//   this->queue_.set_capacity(window_size + 1);
// }

size_t DABALite::size() { return this->queue_class_.size(); }
// size_t DABALite::size() { return this->queue_.size(); }

// insert a new value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  AggregateClass lifted = this->lift_(value);
  this->backSum_ = this->combine_(this->backSum_, lifted);
  this->queue_class_.push_back(lifted);
  this->step_();
}
// // insert a new value at end of circular queue and step DABA Lite algorithm
// void DABALite::insert(float value) {
//   Aggregate lifted = this->lift_(value);
//   this->backSum_ = this->combine_(this->backSum_, lifted);
//   this->queue_.push_back(lifted);
//   this->step_();
// }

// remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  this->queue_class_.pop_front();

  this->step_();
}
// // remove value at start of circular queue and step DABA Lite algorithm
// void DABALite::evict() {
//   this->queue_.pop_front();

//   this->step_();
// }

// Return the summary statistics for all entries in the queue
AggregateClass DABALite::query() {
  if (!this->queue_class_.empty()) {
    AggregateClass alpha = this->get_alpha_();
    AggregateClass back = this->get_back_();

    return this->combine_(alpha, back);
  } else
    return this->identity_class_;
}
// // Return the summary statistics for all entries in the queue
// Aggregate DABALite::query() {
//   if (!this->queue_.empty()) {
//     Aggregate alpha = this->get_alpha_();
//     Aggregate back = this->get_back_();

//     return this->combine_(alpha, back);
//   } else
//     return this->identity_;
// }

// compute summary statistics for a single new value
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
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->queue_class_.head_index() != this->b_) {
    if (this->a_ != this->r_) {
      AggregateClass prev_delta = this->get_delta_();

      this->a_ = this->queue_class_.previous_index(this->a_);
      AggregateClass old_a = this->queue_class_.at_raw(this->a_);

      this->queue_class_.at_raw(this->a_) = combine_(old_a, prev_delta);
    }

    if (this->l_ != this->r_) {
      AggregateClass old_l = this->queue_class_.at_raw(this->l_);
      this->queue_class_.at_raw(this->l_) = combine_(old_l, this->midSum_);

      this->l_ = this->queue_class_.next_index(this->l_);
    } else {
      this->l_ = this->queue_class_.next_index(this->l_);
      this->r_ = this->queue_class_.next_index(this->r_);
      this->a_ = this->queue_class_.next_index(this->a_);
      this->midSum_ = get_delta_();
    }
  } else {
    this->backSum_ = this->midSum_ = this->identity_class_;
  }
}
// void DABALite::step_() {
//   if (this->l_ == this->b_) {
//     this->flip_();
//   }

//   if (this->queue_.head_index() != this->b_) {
//     if (this->a_ != this->r_) {
//       Aggregate prev_delta = this->get_delta_();

//       this->a_ = this->queue_.previous_index(this->a_);
//       Aggregate old_a = this->queue_.at_raw(this->a_);

//       this->queue_.at_raw(this->a_) = combine_(old_a, prev_delta);
//     }

//     if (this->l_ != this->r_) {
//       Aggregate old_l = this->queue_.at_raw(this->l_);
//       this->queue_.at_raw(this->l_) = combine_(old_l, this->midSum_);

//       this->l_ = this->queue_.next_index(this->l_);
//     } else {
//       this->l_ = this->queue_.next_index(this->l_);
//       this->r_ = this->queue_.next_index(this->r_);
//       this->a_ = this->queue_.next_index(this->a_);
//       this->midSum_ = get_delta_();
//     }
//   } else {
//     this->backSum_ = this->midSum_ = this->identity_;
//   }
// }

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->queue_class_.head_index();
  this->r_ = this->b_;
  this->a_ = this->queue_class_.next_index(this->queue_class_.tail_index());
  this->b_ = this->queue_class_.next_index(this->queue_class_.tail_index());

  this->midSum_ = this->backSum_;
  this->backSum_ = this->identity_class_;
}
// // DABA Lite algorithm method
// void DABALite::flip_() {
//   this->l_ = this->queue_.head_index();
//   this->r_ = this->b_;
//   this->a_ = this->queue_.next_index(this->queue_.tail_index());
//   this->b_ = this->queue_.next_index(this->queue_.tail_index());

//   this->midSum_ = this->backSum_;
//   this->backSum_ = this->identity_;
// }

// void DABALite::debug_pointers_() {
//   int head = this->queue_.head_index();
//   int tail = this->queue_.tail_index();
//   int capacity = this->queue_.capacity();

//   size_t f = (head - head);
//   size_t l = (((static_cast<int>(this->l_) - head) % capacity) + capacity) % capacity;
//   size_t r = (((static_cast<int>(this->r_) - head) % capacity) + capacity) % capacity;
//   size_t a = (((static_cast<int>(this->a_) - head) % capacity) + capacity) % capacity;
//   size_t b = (((static_cast<int>(this->b_) - head) % capacity) + capacity) % capacity;
//   size_t e = (((tail - head) % capacity) + capacity) % capacity;

//   ESP_LOGI("iterates (raw)", "f=%d; l=%d; r=%d, a=%d, b=%d, e=%d", head, this->l_, this->r_, this->a_, this->b_,
//   tail); ESP_LOGI("iterates (shifted)", "f=%d; l=%d; r=%d, a=%d, b=%d; e=%d", f, l, r, a, b, e); if ((l > r) || (r >
//   a) || (a > b)) {
//     ESP_LOGE("daba", "pointers in wrong order!");
//   }
// }

// DABA Lite algorithm methods
inline bool DABALite::is_front_empty_() { return this->b_ == this->queue_class_.head_index(); }
inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
inline AggregateClass DABALite::get_back_() { return this->backSum_; }
inline AggregateClass DABALite::get_alpha_() {
  return this->is_front_empty_() ? this->identity_class_ : this->queue_class_.front();
}
inline AggregateClass DABALite::get_delta_() {
  return this->is_delta_empty_() ? this->identity_class_ : this->queue_class_.at_raw(this->a_);
}
// // DABA Lite algorithm methods
// inline bool DABALite::is_front_empty_() { return this->b_ == this->queue_.head_index(); }
// inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
// inline Aggregate DABALite::get_back_() { return this->backSum_; }
// inline Aggregate DABALite::get_alpha_() { return this->is_front_empty_() ? this->identity_ : this->queue_.front(); }
// inline Aggregate DABALite::get_delta_() {
//   return this->is_delta_empty_() ? this->identity_ : this->queue_.at_raw(this->a_);
// }

}  // namespace statistics
}  // namespace esphome
