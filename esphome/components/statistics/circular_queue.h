/*
  Summary Partial statistics are stored in a circular queue with capacity window_size_
  One example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326
  Improved implementation with ideas from
  https://os.mbed.com/users/hamparawa/code/circular_buffer//file/b241b75b052b/circular_buffer.cpp/
*/
#pragma once

#include "esphome/core/helpers.h"
#include <vector>

namespace esphome {
namespace statistics {

template<typename T> class CircularQueue {
 public:
  void set_capacity(size_t capacity);

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

  T &at_raw(size_t index);

  size_t next_index(size_t index);
  size_t previous_index(size_t index);

  size_t head_index();
  size_t tail_index();

 protected:
  std::vector<T, ExternalRAMAllocator<T>> q_{};

  size_t queue_size_{0};
  size_t capacity_{0};

  size_t head_{0};
  size_t tail_{0};

  inline void increment_index_(size_t &index);
};

}  // namespace statistics
}  // namespace esphome
