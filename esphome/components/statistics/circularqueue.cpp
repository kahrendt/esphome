#include "circularqueue.h"

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

}  // namespace statistics
}  // namespace esphome
