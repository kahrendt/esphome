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

struct EnabledAggregatesConfiguration {
  bool count{false};
  bool max{false};
  bool min{false};
  bool mean{false};
  bool m2{false};
  bool timestamp_mean{false};
  bool c2{false};
  bool timestamp_m2{false};
  bool timestamp_reference{false};
};

class Aggregate {
 public:
  Aggregate();  // default constructor for a null measurement
  Aggregate(double value);

  // Count of valid readings; i.e., not NaN, in the set of measurements
  size_t get_count() const { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }

  // Maximum value of the set of measurements
  double get_max() const { return this->max_; }
  void set_max(double max) { this->max_ = max; }

  // minimum value in the set of measurements
  double get_min() const { return this->min_; }
  void set_min(double min) { this->min_ = min; }

  // average value in the set of measurements
  double get_mean() const { return this->mean_; }
  void set_mean(double mean) { this->mean_ = mean; }

  // M2 from Welford's algorithm; used to compute variance
  double get_m2() const { return this->m2_; }
  void set_m2(double m2) { this->m2_ = m2; }

  // C2 from Welford's algorithm; used to compute the covariance of the measurements and timestamps
  double get_c2() const { return this->c2_; }
  void set_c2(double c2) { this->c2_ = c2; }

  // M2 from Welford's algorithm for timestamps; used to compute variance of timestamps
  double get_timestamp_m2() const { return this->timestamp_m2_; }
  void set_timestamp_m2(double timestamp_m2) { this->timestamp_m2_ = timestamp_m2; }

  // The reference timestamp (in milliseconds) that all timestamps in the sum are offset by
  uint32_t get_timestamp_reference() const { return this->timestamp_reference_; }
  void set_timestamp_reference(uint32_t timestamp_reference) { this->timestamp_reference_ = timestamp_reference; }

  double get_timestamp_mean() const { return this->timestamp_mean_; }
  void set_timestamp_mean(double timestamp_mean) { this->timestamp_mean_ = timestamp_mean; }

  // Return the sample variance of measurements
  double compute_variance() const;

  // Return the sample standard deviation of measurements
  double compute_std_dev() const;

  // Return the sample covariance of measurements and timestamps
  double compute_covariance() const;

  // Return the slope of the line of best fit over the window
  double compute_trend() const;

  Aggregate operator+(const Aggregate &b);

 protected:
  /*
   * Default values for the aggregates are the values for a null measurement or the empty set of measurements;
   * i.e., no measurement/observation at all
   */

  // Count of non-NaN measurements in the set of measurements
  size_t count_{0};

  // Extrema of the set of measurements
  double max_{std::numeric_limits<double>::infinity() * (-1)};  // the supremum of the empty set is -infinity
  double min_{std::numeric_limits<double>::infinity()};         // the infimum of the empty set is +infinity

  // Average of the set of measurements
  double mean_{NAN};

  // Quantity used in Welford's algorihtm for finding the variance of the set of measurements
  double m2_{NAN};

  // Quantity used in an extended Welford's algorithm for finding the covariance of the set of measurements and
  // timestamps
  double c2_{NAN};

  // Quantity used in Welford's algorihtm for finding the variance of timestamps in the set of measurements
  double timestamp_m2_{NAN};

  // The reference timestamp for the timestamp sum values;
  // e.g., if we have one raw timestamp at 5 ms and the reference is 5 ms, we store 0 ms in timestamp_sum
  uint32_t timestamp_reference_{0};

  double timestamp_mean_{NAN};

  // Given two samples, normalize the timestamp sums so that they are both in reference to the larger timestamp
  // returns the timestamp both sums are in reference to
  double normalize_timestamp_means_(double &a_mean, const uint32_t &a_timestamp_reference, const size_t &a_count,
                                    double &b_mean, const uint32_t &b_timestamp_reference, const size_t &b_count);
};

template<typename T> class AggregateQueue {
 public:
  virtual bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
  virtual void clear();
  virtual size_t size() const { return 0; };
  virtual void insert(float value);
  virtual void evict(){};
  virtual Aggregate compute_current_aggregate();

  void emplace(const Aggregate &value, size_t index);
  Aggregate lower(size_t index);

  bool allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

 protected:
  size_t *count_queue_{nullptr};
  // float *max_queue_{nullptr};
  // float *min_queue_{nullptr};
  // float *mean_queue_{nullptr};
  // float *m2_queue_{nullptr};
  // float *c2_queue_{nullptr};
  // float *timestamp_m2_queue_{nullptr};
  // float *timestamp_mean_queue_{nullptr};
  T *max_queue_{nullptr};
  T *min_queue_{nullptr};
  T *mean_queue_{nullptr};
  T *m2_queue_{nullptr};
  T *c2_queue_{nullptr};
  T *timestamp_m2_queue_{nullptr};
  T *timestamp_mean_queue_{nullptr};
  uint32_t *timestamp_reference_queue_{nullptr};
};

}  // namespace statistics
}  // namespace esphome
