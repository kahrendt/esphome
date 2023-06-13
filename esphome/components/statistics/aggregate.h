/*
  Aggregate methods for computing summary statistics
    - means are calculated in a manner that hopefully avoids catastrophic cancellation when using a large number of
      samples
    - variance and covariance aggregates are computed using a variation of Welford's alogorithm for parallel computing

  Implemented by Kevin Ahrendt, June 2023
*/

#pragma once

#include <limits>
#include <cmath>

namespace esphome {
namespace statistics {

class Aggregate {
 public:
  // Count of valid readings; i.e., not NaN, in the window
  size_t get_count() const { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }
  void combine_count(const Aggregate &a, const Aggregate &b);

  // maximum value in the window
  float get_max() const { return this->max_; }
  void set_max(float max) { this->max_ = max; }
  void combine_max(const Aggregate &a, const Aggregate &b);

  // minimum value in the window
  float get_min() const { return this->min_; }
  void set_min(float min) { this->min_ = min; }
  void combine_min(const Aggregate &a, const Aggregate &b);

  // average value in the window
  float get_mean() const { return this->mean_; }
  void set_mean(double mean) { this->mean_ = mean; }
  void combine_mean(const Aggregate &a, const Aggregate &b);

  // M2 from Welford's algorithm; used to compute variance
  float get_m2() const { return this->m2_; }
  void set_m2(float m2) { this->m2_ = m2; }
  void combine_m2(const Aggregate &a, const Aggregate &b);

  int32_t get_timestamp_sum() const { return this->timestamp_sum_; }
  void set_timestamp_sum(int32_t timestamp_sum) { this->timestamp_sum_ = timestamp_sum; }
  void combine_timestamp_sum(const Aggregate &a, const Aggregate &b);

  uint32_t get_timestamp_reference() const { return this->timestamp_reference_; }
  void set_timestamp_reference(uint32_t timestamp_reference) { this->timestamp_reference_ = timestamp_reference; }
  void combine_timestamp(const Aggregate &a, const Aggregate &b);

  // C2 from Welford's algorithm; used to compute the covariance of the measurements and timestamps
  float get_c2() const { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void combine_c2(const Aggregate &a, const Aggregate &b);

  // M2 from Welford's algorithm for timestamps; used to compute variance of timestamps
  float get_timestamp_m2() const { return this->timestamp_m2_; }
  void set_timestamp_m2(float timestamp_m2) { this->timestamp_m2_ = timestamp_m2; }
  void combine_timestamp_m2(const Aggregate &a, const Aggregate &b);

  // sample variance of measurements
  float compute_variance() const;

  // sample standard deviation of measurements
  float compute_std_dev() const;

  // sample covariance of measurements and timestamps
  float compute_covariance() const;

  // slope of the line of best fit over the window
  float compute_trend() const;

 protected:
  // default values represent the statistic for a null entry

  // count of non-NaN measurements in the sliding window
  size_t count_{0};

  // extrema of sliding window measurements
  float max_{std::numeric_limits<float>::infinity() * (-1)};  // the supremum of the empty set is -infinity
  float min_{std::numeric_limits<float>::infinity()};         // the infimum of the empty set is +infinity

  // average of sliding window measurements
  float mean_{NAN};

  // Welford's algorithm for finding the variance of sliding window measurements
  float m2_{NAN};

  // Extended Welford's algorithm for finding the covariance of sliding window measurements and timestamps
  float c2_{NAN};

  // Welford's algorithm for finding the variance of timestamps of sliding window measurements
  float timestamp_m2_{NAN};

  // offset timestamps sum of the sliding window measurements
  int32_t timestamp_sum_{0};

  // the reference timestamp for the timestamp sum values
  // e.g., if we have one raw timestamp at 5 ms and the reference is 5 ms, we store 0 ms in the timestamp_sum
  uint32_t timestamp_reference_{0};

  // given two samples, normalize the timestamp sums so that they are both in reference to the larger timestamp
  // returns the timestamp both sums are in reference to
  uint32_t normalize_timestamp_sums_(int32_t &a_timestamp_sum, uint32_t const &a_timestamp, const size_t &a_count,
                                     int32_t &b_timestamp_sum, const uint32_t &b_timestamp, const size_t &b_count);
};

}  // namespace statistics
}  // namespace esphome
