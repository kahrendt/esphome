#include "daba_lite.h"
#include "circular_queue.h"
#include "circular_queue.cpp"
#include <algorithm>

#include "esphome/core/log.h"

namespace esphome {
namespace statistics {

void DABALite::set_window_size(size_t window_size) { this->queue_.set_capacity(window_size + 1); }

size_t DABALite::size() { return this->queue_.size(); }

// insert a new value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  Partial lifted = this->lift_(value);
  this->backSum_ = this->combine_(this->backSum_, lifted);
  this->queue_.push_back(lifted);
  this->step_();
}

// remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  this->queue_.pop_front();

  this->step_();
}

// Return the summary statistics for all entries in the queue
Partial DABALite::query() {
  if (!this->queue_.empty()) {
    Partial alpha = this->get_alpha_();
    Partial back = this->get_back_();

    return this->combine_(alpha, back);
  } else
    return this->identity_;
}

// compute summary statistics for a single new value
Partial DABALite::lift_(float v) {
  Partial part = this->identity_;

  if (!std::isnan(v)) {
    part.max = v;
    part.min = v;

    part.count = 1;

    part.m2 = 0.0;

    part.mean = v;
  }

  return part;
}

// combine summary statistics from two partial samples
Partial DABALite::combine_(Partial &a, Partial &b) {
  Partial part;

  part.max = std::max(a.max, b.max);
  part.min = std::min(a.min, b.min);

  part.count = a.count + b.count;

  double a_count = static_cast<float>(a.count);
  double b_count = static_cast<float>(b.count);
  double part_count = static_cast<double>(part.count);

  if (std::isnan(a.mean) && std::isnan(b.mean)) {
    part.m2 = NAN;
    part.mean = NAN;
  } else if (std::isnan(a.mean)) {  // only a is NAN
    part.m2 = b.m2;
    part.mean = b.mean;
  } else if (std::isnan(b.mean)) {  // only b is NAN
    part.m2 = a.m2;
    part.mean = a.mean;
  } else {  // both exist
    // compute overall M2 using Chan's parallel algorithm for computing the variance
    float delta = b.mean - a.mean;
    part.m2 = a.m2 + b.m2 + delta * delta * a_count * b_count / part_count;

    // weighted average of the two means based on their counts
    part.mean = (a.mean * a_count + b.mean * b_count) / part_count;
  }

  return part;
}

// DABA Lite algorithm method
void DABALite::step_() {
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->queue_.head_index() != this->b_) {
    if (this->a_ != this->r_) {
      Partial prev_delta = this->get_delta_();

      this->a_ = this->queue_.previous_index(this->a_);
      Partial old_a = this->queue_.at_raw(this->a_);

      this->queue_.at_raw(this->a_) = combine_(old_a, prev_delta);
    }

    if (this->l_ != this->r_) {
      Partial old_l = this->queue_.at_raw(this->l_);
      this->queue_.at_raw(this->l_) = combine_(old_l, this->midSum_);

      this->l_ = this->queue_.next_index(this->l_);
    } else {
      this->l_ = this->queue_.next_index(this->l_);
      this->r_ = this->queue_.next_index(this->r_);
      this->a_ = this->queue_.next_index(this->a_);
      this->midSum_ = get_delta_();
    }
  } else {
    this->backSum_ = this->midSum_ = this->identity_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->queue_.head_index();
  this->r_ = this->b_;
  this->a_ = this->queue_.next_index(this->queue_.tail_index());
  this->b_ = this->queue_.next_index(this->queue_.tail_index());

  this->midSum_ = this->backSum_;
  this->backSum_ = this->identity_;
}

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

// online code version
inline bool DABALite::is_front_empty_() { return this->b_ == this->queue_.head_index(); }
inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
inline Partial DABALite::get_back_() { return this->backSum_; }
inline Partial DABALite::get_alpha_() { return this->is_front_empty_() ? this->identity_ : this->queue_.front(); }
inline Partial DABALite::get_delta_() {
  return this->is_delta_empty_() ? this->identity_ : this->queue_.at_raw(this->a_);
}

}  // namespace statistics
}  // namespace esphome
