/*
  Implements an iterator of sorts to keep track of indices in a circular queue
    - Overloads operators to handle increasing and decreasing the index by 1
    - Should work on any array like structure

  An example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326

  Implemented by Kevin Ahrendt, June 2023
*/

#pragma once

#include "esphome/core/helpers.h"

namespace esphome {
namespace statistics {

class CircularQueueIndex {
 public:
  // default constructor
  CircularQueueIndex() {
    this->index_ = 0;
    this->capacity_ = 0;
  }

  // typical constructor
  CircularQueueIndex(size_t index, size_t capacity) {
    this->index_ = index;
    this->capacity_ = capacity;
  }

  void set_index(size_t index) { this->index_ = index; }
  size_t get_index() const { return this->index_; }

  void set_capacity(size_t capacity) { this->capacity_ = capacity; }
  size_t get_capacity() const { return this->capacity_; }

  // overloaded prefix increment operator
  CircularQueueIndex &operator++();

  // overloaded prefix decrement operator
  CircularQueueIndex &operator--();

  // overloaded equality operator
  CircularQueueIndex &operator=(const CircularQueueIndex &i);

  // overloaded equality comparison operator
  bool operator==(const CircularQueueIndex &i) const;

  // overlaoded inequality comparison operator
  bool operator!=(const CircularQueueIndex &i) const;

 private:
  // index of value in circular queue
  size_t index_;

  // capacity of the circular queue
  size_t capacity_;
};

}  // namespace statistics
}  // namespace esphome
