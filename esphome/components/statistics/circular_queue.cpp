#include "circular_queue.h"
#include <vector>

#include "esphome/core/log.h"

namespace esphome {
namespace statistics {

template<typename T> void CircularQueue<T>::set_capacity(size_t capacity) {
  this->capacity_ = capacity;
  this->q_.reserve(this->capacity_);
}

template<typename T> bool CircularQueue<T>::empty() { return this->queue_size_ == 0; }
template<typename T> size_t CircularQueue<T>::size() { return this->queue_size_; }
template<typename T> size_t CircularQueue<T>::max_size() { return this->capacity_; }
template<typename T> size_t CircularQueue<T>::capacity() { return this->q_.capacity(); }

// push a new value at end of circular queue
template<typename T> void CircularQueue<T>::push_back(T value) {
  if (!this->empty())
    increment_index_(this->tail_);

  this->q_[this->tail_] = value;

  ++this->queue_size_;
}

// pop off value at front of circular queue
template<typename T> void CircularQueue<T>::pop_front() {
  if (!this->empty()) {
    this->increment_index_(head_);
    --this->queue_size_;
  }
}

template<typename T> size_t CircularQueue<T>::front_index() { return 0; }
template<typename T> size_t CircularQueue<T>::back_index() { return this->queue_size_ - 1; }

template<typename T> T &CircularQueue<T>::front() { return this->q_[this->head_]; }
template<typename T> T &CircularQueue<T>::back() { return this->q_[this->back_index()]; }

template<typename T> T &CircularQueue<T>::at_raw(size_t index) { return this->q_[index]; }

template<typename T> size_t CircularQueue<T>::next_index(size_t index) {
  if (index == (this->capacity_ - 1))
    return 0;

  return ++index;
}
template<typename T> size_t CircularQueue<T>::previous_index(size_t index) {
  if (index == 0)
    return this->capacity_ - 1;

  return --index;
}

template<typename T> size_t CircularQueue<T>::head_index() { return this->head_; }
template<typename T> size_t CircularQueue<T>::tail_index() { return this->tail_; }

// Circular Queue - next index following circulr queue logic
template<typename T> void CircularQueue<T>::increment_index_(size_t &index) { index = this->next_index(index); }

// // retrieve element at an index
// template<typename T> T &CircularQueue<T>::at(size_t index) {
//   size_t real_index = (index + this->head_) % this->capacity_;

//   return q_[real_index];
// }

// // overload subscript operator for the circular queue
// template<typename T> T &CircularQueue<T>::operator[](size_t index) { return this->at(index); }

}  // namespace statistics
}  // namespace esphome
