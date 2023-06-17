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

  // general constructor
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
  size_t index_;     // index of value in circular queue
  size_t capacity_;  // capacity of the circular queue
};

}  // namespace statistics
}  // namespace esphome
