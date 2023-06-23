/*
 * Aggregate class for computing summary statistics for a set of measurements
 *  - Three main roles:
 *    1) Set a default value for a null measurement; i.e., for an empty set of measurements
 *    2) Combine two aggregates from two disjoint sets of measurements
 *    3) Compute summary statistics from the stored aggregates
 *
 *  - Means are calculated in a manner that avoids catastrophic cancellation with a large number of measurements.
 *  - Variance and covariance aggregates are computed using a variation of Welford's alogorithm for parallel computing.
 *    - See the article "Numerically Stable Parallel Computation of (Co-)Variance" by Schubert and Gertz for details.
 *  - Two timestamp quantities are stored: timestamp_sum & timestamp_reference
 *    - timestamp_sum is the sum of the timestamps (in milliseconds) in the set of measurements all offset by the
 * reference.
 *    - timestamp_reference is the offset (in milliseconds) is the offset for timestamp_sum.
 *    - timestamp_sums need to normalized before comparing so that they reference the same timestamp
 *      - The normalizing process involves finding the time delta between the two references; this avoids issues from
 *        millis() rolling over
 *    - This approach ensures one of timestamps included in the timestamp_sum is 0.
 *      - timestamp_sum is as small as possible to minimize floating point operations losing significant digits.
 *    - timestamp_sum and timestamp_reference are stored as integers.
 *      - Operations on integers are performed as much as possible before switching to (slower) floating point
 *          operations.
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include <cmath>   // necessary for NaN
#include <limits>  // necessary for std::numeric_limits infinity

namespace esphome {
namespace statistics {

class Aggregate {
 public:
  // Count of valid readings; i.e., not NaN, in the set of measurements
  size_t get_count() const { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }
  void combine_count(const Aggregate &a, const Aggregate &b);

  // Maximum value of the set of measurements
  float get_max() const { return this->max_; }
  void set_max(float max) { this->max_ = max; }
  void combine_max(const Aggregate &a, const Aggregate &b);

  // minimum value in the set of measurements
  float get_min() const { return this->min_; }
  void set_min(float min) { this->min_ = min; }
  void combine_min(const Aggregate &a, const Aggregate &b);

  // average value in the set of measurements
  float get_mean() const { return this->mean_; }
  void set_mean(double mean) { this->mean_ = mean; }
  void combine_mean(const Aggregate &a, const Aggregate &b);

  // M2 from Welford's algorithm; used to compute variance
  float get_m2() const { return this->m2_; }
  void set_m2(float m2) { this->m2_ = m2; }
  void combine_m2(const Aggregate &a, const Aggregate &b);

  // C2 from Welford's algorithm; used to compute the covariance of the measurements and timestamps
  float get_c2() const { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void combine_c2(const Aggregate &a, const Aggregate &b);

  // M2 from Welford's algorithm for timestamps; used to compute variance of timestamps
  float get_timestamp_m2() const { return this->timestamp_m2_; }
  void set_timestamp_m2(float timestamp_m2) { this->timestamp_m2_ = timestamp_m2; }
  void combine_timestamp_m2(const Aggregate &a, const Aggregate &b);

  // Offset sum (by the reference) of the timestamps in the set
  int32_t get_timestamp_sum() const { return this->timestamp_sum_; }
  void set_timestamp_sum(int32_t timestamp_sum) { this->timestamp_sum_ = timestamp_sum; }
  void combine_timestamp_sum(const Aggregate &a, const Aggregate &b);

  // The reference timestamp (in milliseconds) that all timestamps in the sum are offset by
  uint32_t get_timestamp_reference() const { return this->timestamp_reference_; }
  void set_timestamp_reference(uint32_t timestamp_reference) { this->timestamp_reference_ = timestamp_reference; }
  void combine_timestamp(const Aggregate &a, const Aggregate &b);

  // Return the sample variance of measurements
  float compute_variance() const;

  // Return the sample standard deviation of measurements
  float compute_std_dev() const;

  // Return the sample covariance of measurements and timestamps
  float compute_covariance() const;

  // Return the slope of the line of best fit over the window
  float compute_trend() const;

 protected:
  /*
   * Default values for the aggregates are the values for a null measurement or the empty set of measurements;
   * i.e., no measurement/observation at all
   */

  // Count of non-NaN measurements in the set of measurements
  size_t count_{0};

  // Extrema of the set of measurements
  float max_{std::numeric_limits<float>::infinity() * (-1)};  // the supremum of the empty set is -infinity
  float min_{std::numeric_limits<float>::infinity()};         // the infimum of the empty set is +infinity

  // Average of the set of measurements
  float mean_{NAN};

  // Quantity used in Welford's algorihtm for finding the variance of the set of measurements
  float m2_{NAN};

  // Quantity used in an extended Welford's algorithm for finding the covariance of the set of measurements and
  // timestamps
  float c2_{NAN};

  // Quantity used in Welford's algorihtm for finding the variance of timestamps in the set of measurements
  float timestamp_m2_{NAN};

  // Sum of all the timestamps in the set of measurements - offset by the reference
  int32_t timestamp_sum_{0};

  // The reference timestamp for the timestamp sum values;
  // e.g., if we have one raw timestamp at 5 ms and the reference is 5 ms, we store 0 ms in timestamp_sum
  uint32_t timestamp_reference_{0};

  // Given two samples, normalize the timestamp sums so that they are both in reference to the larger timestamp
  // returns the timestamp both sums are in reference to
  uint32_t normalize_timestamp_sums_(int32_t &a_timestamp_sum, uint32_t const &a_timestamp, const size_t &a_count,
                                     int32_t &b_timestamp_sum, const uint32_t &b_timestamp, const size_t &b_count);
};

}  // namespace statistics
}  // namespace esphome
