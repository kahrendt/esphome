/*
 * Class that handles an index for a circular queue
 *   - Circular queue concept:
 *     - Has a capacity set at the start
 *     - The index of the next element past the end of the array structure
 *       is the first index of the array structure; i.e., it loops around
 *     - An example implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326
 *   - Overloads operators to handle index operations respecting the circular queue structure
 *   - Should work on any array like structure with element access
 *
 * Implemented by Kevin Ahrendt, June 2023
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
