#pragma once

#include <cstdint>
#include <vector>

namespace esphome {
namespace snapcast {

class MedianFilter {
 public:
  /// @brief Constructs a new filter with a specified sample capacity. Computes a sliding window median over the
  /// samples.
  /// @param capacity Number of samples in the sliding window
  MedianFilter(uint8_t capacity) : capacity_(capacity) { this->data_.reserve(capacity); };

  /// @brief Adds a new value to the filter. If the filter is at capacity, it overwrites the oldest sample.
  /// @param new_value
  /// @return The new median value
  int64_t update(int64_t new_value);

  /// @brief Gets the most recently updated median value.
  /// @return Median value
  int64_t get_most_recent_median() const { return this->most_recent_median_; }

  /// @brief Tests if the filter is filled to its sample capacity.
  /// @return True if there are ``capacity_`` samples stored, false otherwise
  bool is_full() const { return (this->data_.size() == this->capacity_); }

  /// @brief Clears all old data, resets the index, and sets the most recent median to 0.
  void reset();

 protected:
  std::vector<int64_t> data_;
  int64_t most_recent_median_{0};
  uint8_t index_{0};
  uint8_t capacity_;
};

}  // namespace snapcast
}  // namespace esphome
