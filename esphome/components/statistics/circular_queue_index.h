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
  CircularQueueIndex() {
    this->index_ = 0;
    this->capacity_ = 0;
  }
  CircularQueueIndex(size_t index, size_t capacity) {
    this->index_ = index;
    this->capacity_ = capacity;
  }

  void set_index(size_t index) { this->index_ = index; }
  size_t get_index() const { return this->index_; }

  void set_capacity(size_t capacity) { this->capacity_ = capacity; }
  size_t get_capacity() const { return this->capacity_; }

  CircularQueueIndex &operator++();

  // const CircularQueueIndex operator++(int);

  CircularQueueIndex &operator--();

  // const CircularQueueIndex operator--(int);

  CircularQueueIndex &operator=(const CircularQueueIndex &i);

  bool operator==(const CircularQueueIndex &i) const;

  bool operator!=(const CircularQueueIndex &i) const;

 private:
  size_t index_;
  size_t capacity_;
};

}  // namespace statistics
}  // namespace esphome
