#include "median_filter.h"

#include <algorithm>

namespace esphome {
namespace snapcast {

int64_t MedianFilter::update(int64_t new_value) {
  // If not at capacity, add the new sample to the end. Otherwise, overwrite the oldest sample.
  if (this->data_.size() < this->capacity_) {
    this->data_.push_back(new_value);
  } else {
    this->data_[this->index_] = new_value;
  }

  // Update the index, wrapping around to the beginning if at capacity.
  ++this->index_;
  if (this->index_ == this->capacity_) {
    this->index_ = 0;
  }

  // Copy the data and sort the copy.
  std::vector<int64_t> sorted_data(this->data_);
  std::sort(sorted_data.begin(), sorted_data.end());

  // Compute the median value. If the number of samples is even, take the average of the two middle values. Otherwise,
  // use the middle value.
  if (sorted_data.size() % 2 == 0) {
    this->most_recent_median_ = sorted_data[sorted_data.size() / 2] / 2 + sorted_data[sorted_data.size() / 2 + 1] / 2;
  } else {
    this->most_recent_median_ = sorted_data[sorted_data.size() / 2];
  }

  return this->most_recent_median_;
}

void MedianFilter::reset() {
  this->data_.clear();
  this->index_ = 0;
  this->most_recent_median_ = 0;
}

}  // namespace snapcast
}  // namespace esphome
