/*
  Implements an iterator of sorts to keep track of indices in a circular queue
    - Overloads operators to handle increasing and decreasing the index by 1
    - Should work on any array like structure

  An example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326

  To-do:
    - move function implementations over to circular_queue_index.cpp
*/
#pragma once

#include <vector>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

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
  size_t get_index() { return this->index_; }

  void set_capacity(size_t capacity) { this->capacity_ = capacity; }
  size_t get_capacity() { return this->capacity_; }

  CircularQueueIndex &operator++() {
    if (this->index_ == (this->capacity_ - 1)) {
      this->index_ = 0;
      return *this;
    }

    ++this->index_;

    // ESP_LOGI("cq increment", "index=%d;capacity=%d", this->index_, this->capacity_);
    return *this;
  }

  CircularQueueIndex operator++(int) {
    if (this->index_ == (this->capacity_ - 1))
      return CircularQueueIndex(0, this->capacity_);

    return CircularQueueIndex(++this->index_, this->capacity_);
  }

  CircularQueueIndex &operator--() {
    if (this->index_ == 0) {
      this->index_ = this->capacity_ - 1;
      return *this;
    }

    --this->index_;
    return *this;
  }

  CircularQueueIndex operator--(int) {
    if (this->index_ == 0)
      return CircularQueueIndex(this->capacity_ - 1, this->capacity_);

    return CircularQueueIndex(--this->index_, this->capacity_);
  }

  void operator=(const CircularQueueIndex &i) {
    this->index_ = i.index_;
    // get_index();
    this->capacity_ = i.capacity_;  // get_capacity();
  }

  bool operator==(CircularQueueIndex &i) {
    if ((this->index_ == i.get_index()) && this->capacity_ == i.get_capacity())
      return true;

    return false;
  }

  bool operator!=(CircularQueueIndex &i) {
    if ((this->index_ != i.get_index()) || this->capacity_ != i.get_capacity())
      return true;

    return false;
  }

 private:
  size_t index_;
  size_t capacity_;
};

}  // namespace statistics
}  // namespace esphome
