#include "dabalite.h"
#include <algorithm>

namespace esphome {

namespace statistics {

template<typename T> void CircularQueue<T>::set_capacity(size_t capacity) {
  this->capacity_ = capacity;
  this->q_.reserve(this->capacity_);
}

template<typename T> size_t CircularQueue<T>::size() { return this->queue_size_; }

// push a new value at end of circular queue
template<typename T> void CircularQueue<T>::push_back(T value) {
  if (this->size() > 0)
    this->tail_ = increment_index_(this->tail_);

  this->q_[this->tail_] = value;

  ++this->queue_size_;
}

// pop off value at front of circular queue
template<typename T> void CircularQueue<T>::pop_front() {
  if (this->size() > 0) {
    this->head_ = this->increment_index_(head_);
    --this->queue_size_;
  }
}

// Circular Queue - next index following circulr queue logic
template<typename T> inline size_t CircularQueue<T>::increment_index_(size_t index) {
  return (index + 1) % this->capacity_;
}

// retrieve element at an index
template<typename T> T &CircularQueue<T>::at(size_t index) {
  size_t real_index = (index + this->head_) % this->capacity_;

  return q_[real_index];
}

// overload subscript operator for the circular queue
template<typename T> T &CircularQueue<T>::operator[](size_t index) { return this->at(index); }

void DABALite::set_window_size(size_t window_size) { this->queue_.set_capacity(window_size); }

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
  if (this->size() > 0) {
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

  // compute overall M2 using Chan's parallel algorithm for computing the variance
  float delta = b.mean - a.mean;
  part.m2 = a.m2 + b.m2 + delta * delta * a_count * b_count / part_count;

  // weighted average of the two means based on their counts
  part.mean = (a.mean * a_count + b.mean * b_count) / part_count;

  return part;
}

// DABA Lite algorithm method
void DABALite::step_() {
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->b_ != 0) {
    if (a_ != r_) {
      Partial prev_delta = this->get_delta_();

      --this->a_;

      Partial old_a = this->queue_[this->a_];
      this->queue_[this->a_] = combine_(old_a, prev_delta);
    }

    if (this->l_ != this->r_) {
      Partial old_l = this->queue_[this->l_];
      this->queue_[this->l_] = combine_(old_l, this->midSum_);

      ++this->l_;
    } else {
      ++this->l_;
      ++this->r_;
      ++this->a_;
      this->midSum_ = get_delta_();
    }
  } else {
    this->backSum_ = this->midSum_ = this->identity_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = 0;
  this->r_ = this->b_;
  this->a_ = this->queue_.size();
  this->b_ = this->queue_.size();

  this->midSum_ = this->backSum_;
  this->backSum_ = this->identity_;
}

// DABA Lite algorithm methods
inline bool DABALite::is_front_empty_() { return this->b_ == 0; }
inline bool DABALite::is_delta_empty_() { return this->a_ == this->b_; }
inline Partial DABALite::get_back_() { return this->backSum_; }
inline Partial DABALite::get_alpha_() { return is_front_empty_() ? this->identity_ : this->queue_[0]; }
inline Partial DABALite::get_delta_() { return is_delta_empty_() ? this->identity_ : this->queue_[this->a_]; }

}  // namespace statistics
}  // namespace esphome
