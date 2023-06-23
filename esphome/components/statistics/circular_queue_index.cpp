/*
 * Class that handles an index for a circular queue
 *  - Circular queue concept:
 *    - Has a capacity set at the start
 *    - The index of the next element past the end of the array structure
 *       is the first index of the array structure; i.e., it loops around
 *    - An example implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326
 *  - Overloads operators to handle index operations respecting the circular queue structure
 *  - Should work on any array like structure with element access
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#include "circular_queue_index.h"

namespace esphome {
namespace statistics {

// Default constructor
CircularQueueIndex::CircularQueueIndex() {
  this->index_ = 0;
  this->capacity_ = 0;
}

// General constructor
CircularQueueIndex::CircularQueueIndex(size_t index, size_t capacity) {
  this->index_ = index;
  this->capacity_ = capacity;
}

// Overloaded prefix increment operator
CircularQueueIndex &CircularQueueIndex::operator++() {
  if (this->index_ == (this->capacity_ - 1)) {
    this->index_ = 0;
    return *this;
  }

  ++this->index_;
  return *this;
}

// Overloaded prefix decrement operator
CircularQueueIndex &CircularQueueIndex::operator--() {
  if (this->index_ == 0) {
    this->index_ = this->capacity_ - 1;
    return *this;
  }

  --this->index_;
  return *this;
}

// Overloaded equality operator
CircularQueueIndex &CircularQueueIndex::operator=(const CircularQueueIndex &i) {
  if (this == &i)
    return *this;
  this->index_ = i.index_;
  this->capacity_ = i.capacity_;

  return *this;
}

// Overloaded equality comparison operator
bool CircularQueueIndex::operator==(const CircularQueueIndex &i) const {
  return (this->index_ == i.get_index()) && this->capacity_ == i.get_capacity();
}

// Overloaded inequality comparison operator
bool CircularQueueIndex::operator!=(const CircularQueueIndex &i) const {
  return (this->index_ != i.get_index()) || this->capacity_ != i.get_capacity();
}

}  // namespace statistics
}  // namespace esphome
