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

CircularQueueIndex &CircularQueueIndex::operator++() {
  if (this->index_ == (this->capacity_ - 1)) {
    this->index_ = 0;
    return *this;
  }

  ++this->index_;
  return *this;
}

CircularQueueIndex CircularQueueIndex::operator++(int) {
  if (this->index_ == (this->capacity_ - 1))
    return CircularQueueIndex(0, this->capacity_);

  return CircularQueueIndex(++this->index_, this->capacity_);
}

CircularQueueIndex &CircularQueueIndex::operator--() {
  if (this->index_ == 0) {
    this->index_ = this->capacity_ - 1;
    return *this;
  }

  --this->index_;
  return *this;
}

CircularQueueIndex CircularQueueIndex::operator--(int) {
  if (this->index_ == 0)
    return CircularQueueIndex(this->capacity_ - 1, this->capacity_);

  return CircularQueueIndex(--this->index_, this->capacity_);
}

CircularQueueIndex &CircularQueueIndex::operator=(const CircularQueueIndex &i) {
  if (this == &i)
    return *this;
  this->index_ = i.index_;
  this->capacity_ = i.capacity_;

  return *this;
}

bool CircularQueueIndex::operator==(CircularQueueIndex &i) {
  return (this->index_ == i.get_index()) && this->capacity_ == i.get_capacity();
}

bool CircularQueueIndex::operator!=(CircularQueueIndex &i) {
  return (this->index_ != i.get_index()) || this->capacity_ != i.get_capacity();
}

}  // namespace statistics
}  // namespace esphome
