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

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

namespace esphome {
namespace statistics {

// Set capacity (and reserve in memory) of the circular queues for the desired statistics
//  - returns whether memory was successfully allocated
template<typename T> bool DABALite<T>::set_capacity(size_t window_size, EnabledAggregatesConfiguration config) {
  this->window_size_ = window_size;

  if (!this->allocate_memory(this->window_size_, config))
    return false;

  this->clear();

  return true;
}

template<typename T> void DABALite<T>::clear() {
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
template<typename T> void DABALite<T>::insert(T value, uint32_t time_delta) {
  Aggregate lifted = Aggregate(value, time_delta);

  this->back_sum_ = this->back_sum_ + lifted;

  this->emplace(lifted, this->e_.get_index());

  ++this->e_;
  ++this->size_;
  this->step_();
}

// Remove value at start of circular queue and step DABA Lite algorithm
template<typename T> void DABALite<T>::evict() {
  ++this->f_;
  --this->size_;

  this->step_();
}

template<typename T> Aggregate DABALite<T>::compute_current_aggregate() {
  if (this->size() > 0) {
    Aggregate alpha = this->get_alpha_();
    Aggregate back = this->get_back_();

    return alpha + back;
  }
  return this->identity_class_;
}

// DABA Lite algorithm method
template<typename T> void DABALite<T>::step_() {
  // this->debug_pointers_();
  if (this->l_ == this->b_) {
    this->flip_();
  }

  if (this->size_ > 0) {
    if (this->a_ != this->r_) {
      Aggregate prev_delta = this->get_delta_();

      --this->a_;
      Aggregate old_a = this->lower(this->a_.get_index());

      this->emplace(old_a + prev_delta, this->a_.get_index());
    }

    if (this->l_ != this->r_) {
      Aggregate old_l = this->lower(this->l_.get_index());

      this->emplace(old_l + this->mid_sum_, this->l_.get_index());
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
template<typename T> void DABALite<T>::flip_() {
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
template<typename T> inline bool DABALite<T>::is_front_empty_() {
  return (this->b_ == this->f_) && (this->size_ != this->window_size_);
}

template<typename T> inline bool DABALite<T>::is_delta_empty_() { return this->a_ == this->b_; }
template<typename T> inline Aggregate DABALite<T>::get_back_() { return this->back_sum_; }
template<typename T> inline Aggregate DABALite<T>::get_alpha_() {
  return this->is_front_empty_() ? this->identity_class_ : this->lower(this->f_.get_index());
}
template<typename T> inline Aggregate DABALite<T>::get_delta_() {
  return this->is_delta_empty_() ? this->identity_class_ : this->lower(this->a_.get_index());
}

// avoids linking errors (https://isocpp.org/wiki/faq/templates)
template bool DABALite<float>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void DABALite<float>::clear();
template size_t DABALite<float>::size() const;
template void DABALite<float>::insert(float value, uint32_t time_delta);
template void DABALite<float>::evict();
template Aggregate DABALite<float>::compute_current_aggregate();

template bool DABALite<double>::set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
template void DABALite<double>::clear();
template size_t DABALite<double>::size() const;
template void DABALite<double>::insert(double value, uint32_t time_delta);
template void DABALite<double>::evict();
template Aggregate DABALite<double>::compute_current_aggregate();

}  // namespace statistics
}  // namespace esphome
