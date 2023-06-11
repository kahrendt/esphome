/*
  Implements an iterator of sorts to keep track of indices in a circular queue
    - Overloads operators to handle increasing and decreasing the index by 1
    - Should work on any array like structure

  An example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326

  Implemented by Kevin Ahrendt, June 2023
*/

#include "circular_queue_index.h"

namespace esphome {
namespace statistics {

// overloaded prefix increment operator
CircularQueueIndex &CircularQueueIndex::operator++() {
  if (this->index_ == (this->capacity_ - 1)) {
    this->index_ = 0;
    return *this;
  }

  ++this->index_;
  return *this;
}

// overloaded prefix decrement operator
CircularQueueIndex &CircularQueueIndex::operator--() {
  if (this->index_ == 0) {
    this->index_ = this->capacity_ - 1;
    return *this;
  }

  --this->index_;
  return *this;
}

// overloaded equality operator
CircularQueueIndex &CircularQueueIndex::operator=(const CircularQueueIndex &i) {
  if (this == &i)
    return *this;
  this->index_ = i.index_;
  this->capacity_ = i.capacity_;

  return *this;
}

// overloaded equality comparison operator
bool CircularQueueIndex::operator==(const CircularQueueIndex &i) const {
  return (this->index_ == i.get_index()) && this->capacity_ == i.get_capacity();
}

// overlaoded inequality comparison operator
bool CircularQueueIndex::operator!=(const CircularQueueIndex &i) const {
  return (this->index_ != i.get_index()) || this->capacity_ != i.get_capacity();
}

}  // namespace statistics
}  // namespace esphome
