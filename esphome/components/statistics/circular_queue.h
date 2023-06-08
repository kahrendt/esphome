/*
  Summary Partial statistics are stored in a circular queue with capacity window_size_
  One example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326
  Improved implementation with ideas from
  https://os.mbed.com/users/hamparawa/code/circular_buffer//file/b241b75b052b/circular_buffer.cpp/
*/
#pragma once

#include "esphome/core/helpers.h"
#include <vector>
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

template<typename T> class CircularQueue {
 public:
  void set_capacity_and_fill(size_t capacity, T fill);

  bool empty();
  size_t size();
  size_t max_size();
  size_t capacity();

  void push_back(T value);
  void pop_front();

  size_t front_index();
  size_t back_index();

  T &front();
  T &back();

  // T &at(size_t index);
  // T &operator[](size_t index);

  T &at_raw(size_t raw_index);

  size_t next_index(size_t index);
  size_t previous_index(size_t index);

  size_t head_index();
  size_t tail_index();

 protected:
  // std::vector<T> q_{};
  std::vector<T, ExternalRAMAllocator<T>> q_{};

  size_t queue_size_{0};
  size_t capacity_{0};

  size_t head_{0};
  size_t tail_{0};

  inline void increment_index_(size_t &index);
};

}  // namespace statistics
}  // namespace esphome
