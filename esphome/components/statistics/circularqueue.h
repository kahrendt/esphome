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

  size_t size();

  void push_back(T value);
  void pop_front();

  T &at(size_t index);
  T &operator[](size_t index);

 protected:
  std::vector<T, ExternalRAMAllocator<T>> q_{};

  size_t queue_size_{0};
  size_t capacity_{0};

  size_t head_{0};
  size_t tail_{0};

  inline size_t increment_index_(size_t index);
};

}  // namespace statistics
}  // namespace esphome
