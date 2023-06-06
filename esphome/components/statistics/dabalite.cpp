#include "dabalite.h"
#include <algorithm>

namespace esphome {

namespace statistics {
  
template <typename T> CircularQueue<T>::CircularQueue(size_t window_size) {
  this->window_size_ = window_size;
  this->q_.reserve(this->window_size_);
}

template <typename T> size_t CircularQueue<T>::size() { return this->queue_size_; }

template <typename T> size_t CircularQueue<T>::front() { return this->head_; }
template <typename T> size_t CircularQueue<T>::back() { return this->tail_; }

// insert a new value at end of circular queue and step DABA Lite algorithm
template <typename T> void CircularQueue<T>::insert(T value) {
  if (this->size() > 0)
    this->tail_ = next_index(this->tail_);

  this->q_[this->tail_] = value;

  ++this->queue_size_;
}

template <typename T> void CircularQueue<T>::evict() {
  if (this->size() > 0) {
    this->head_ = this->next_index(head_);
    --this->queue_size_;
  }
}


template <typename T> T CircularQueue<T>::retrieve(size_t index) {
  return this->q_[index];
}

template <typename T> void CircularQueue<T>::replace(size_t index, T value) {
  this->q_[index] = value;
}

// Circular Queue - next and previous indices in the circular queue
template <typename T> inline size_t CircularQueue<T>::next_index(size_t current) { return (current + 1) % this->window_size_; }
template <typename T> inline size_t CircularQueue<T>::previous_index(size_t current) { return (current - 1) % this->window_size_; }


DABALite::DABALite(size_t window_size) {
  this->queue_ = new CircularQueue<Partial>(window_size);
}

size_t DABALite::size() { return this->queue_->size(); }

// insert a new value at end of circular queue and step DABA Lite algorithm
void DABALite::insert(float value) {
  Partial lifted = this->lift_(value);

  this->backSum_ = this->combine_(this->backSum_, lifted);

  this->queue_->insert(lifted);

  this->step_();
}

// remove value at start of circular queue and step DABA Lite algorithm
void DABALite::evict() {
  this->queue_->evict();

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

  if (this->queue_->front() != this->b_) {
    if (a_ != r_) {
      Partial prev_delta = this->get_delta_();

      this->a_ = this->queue_->previous_index(this->a_);

      Partial old_a = this->queue_->retrieve(this->a_);

      this->queue_->replace(this->a_, combine_(old_a, prev_delta));
    }

    if (this->l_ != this->r_) {
      Partial old_l = this->queue_->retrieve(this->l_);
      this->queue_->replace(this->l_, combine_(old_l, this->midSum_));

      this->l_ = this->queue_->next_index(this->l_);
    } else {
      this->l_ = this->queue_->next_index(this->l_);
      this->r_ = this->queue_->next_index(this->r_);
      this->a_ = this->queue_->next_index(this->a_);
      this->midSum_ = get_delta_();
    }
  } else {
    this->backSum_ = this->midSum_ = this->identity_;
  }
}

// DABA Lite algorithm method
void DABALite::flip_() {
  this->l_ = this->queue_->front();
  this->r_ = this->b_;
  this->a_ = this->queue_->back();
  this->b_ = this->queue_->back();

  this->midSum_ = this->backSum_;
  this->backSum_ = this->identity_;
}

// DABA Lite algorithm methods
inline bool DABALite::is_front_empty() { return this->b_ == this->queue_->front(); }
inline bool DABALite::is_delta_empty() { return this->a_ == this->b_; }
inline Partial DABALite::get_back_() { return this->backSum_; }
inline Partial DABALite::get_alpha_() { return is_front_empty() ? this->identity_ : this->queue_->retrieve(this->queue_->front()); }
inline Partial DABALite::get_delta_() { return is_delta_empty() ? this->identity_ : this->queue_->retrieve(this->a_); }

}  // namespace statistics
}  // namespace esphome
