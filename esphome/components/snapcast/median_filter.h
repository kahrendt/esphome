#pragma once

#include <cstdint>
#include <vector>

namespace esphome {
namespace snapcast {

class MedianFilter {
 public:
  MedianFilter(uint8_t capacity) : capacity_(capacity) { this->data_.reserve(capacity); };

  int64_t update(int64_t new_value) {
    if (this->data_.size() < this->capacity_) {
      this->data_.push_back(new_value);
    } else {
      this->data_[this->index_] = new_value;
    }
    ++this->index_;
    if (this->index_ == this->capacity_) {
      this->index_ = 0;
    }

    std::vector<int64_t> sorted_data;
    for (const int64_t &value : this->data_) {
      sorted_data.push_back(value);
    }
    std::sort(sorted_data.begin(), sorted_data.end());

    int64_t median_offset = sorted_data[sorted_data.size() / 2];
    if (sorted_data.size() % 2 == 0) {
      median_offset = sorted_data[sorted_data.size() / 2] / 2 + sorted_data[sorted_data.size() / 2 + 1] / 2;
    }

    this->most_recent_median_ = median_offset;
    return median_offset;
  }

  int64_t get_most_recent_median() { return this->most_recent_median_; }

  bool is_full() { return (this->data_.size() == this->capacity_); }

  void reset() {
    this->data_.clear();
    this->most_recent_median_ = 0;
  }

 protected:
  int64_t most_recent_median_{0};
  std::vector<int64_t> data_;
  uint8_t index_{0};
  uint8_t capacity_;
};

}  // namespace snapcast
}  // namespace esphome
