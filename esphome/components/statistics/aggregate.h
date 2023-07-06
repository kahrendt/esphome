/*
 * Aggregate class stores summary statistics for a set of measurements. It is mathematically a monoid paired with a
 * binary operation and an identity. It has three main functions
 *  - Set a default value for a null measurement (a set with 0 measurements) - Aggregate method
 *    - this is the identity monoid
 *  - Combine two aggregates from two disjoint sets of measurements - combine_with method
 *    - this is the binary operation
 *    - summary statistics are combined using parallel algorithms
 *      - See the article "Numerically Stable Parallel Computation of (Co-)Variance" by Schubert and Gertz for details.
 *  - Compute summary statistics from the stored aggregates - get_* and compute_* methods
 *    - Some summary statistics are directly stored
 *      - count, duration, min, mean, and max aggrates are directly stored
 *      - use get_* methods for retrieval
 *    - Some of the stored summary statistics in the monoid are not useful immediately, but can be combined with other
 *      stored measurements to compute the useful statistic
 *      - variance and standard deviation are computed using the stored count (or duration if time-weighted) and m2
 *        statistics
 *        - m2 additionally requires the mean statistic for the combine operation
 *      - covariance is computed using the stored count (or duration if time-weighted) and c2 statistics
 *        - c2 additionally requires the mean and timestamp mean for the combine operation
 *      - trend is computed using c2 and timestamp m2 statistics
 *        - timestamp m2 statistic additionally requies the timestamp mean and count (or duration if time-weighted)
 *          for the combine operation
 *
 * For any statistic that uses timestamp_mean, the aggregate also stores timestamp_reference
 *  - timestamp_reference is the offset (in milliseconds) for the timestamp_mean
 *  - timestamp_mean's need to normalized before combined so that they reference the same timestamp
 *    - The normalizing process involves finding the time delta between the two references; this avoids issues from
 *      millis() rolling over
 *  - This approach ensures one fo the timestamp_reference's is 0 when combining two aggregates
 *    - This makes timestamp_means as small as possible to minimize floating point precision issues
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
 */

#pragma once

#include <cmath>   // necessary for NaN
#include <limits>  // necessary for std::numeric_limits infinity

namespace esphome {
namespace statistics {

enum GroupType {
  SAMPLE_GROUP_TYPE,
  POPULATION_GROUP_TYPE,
};

class Aggregate {
 public:
  Aggregate(){};  // default constructor for a null measurement
  Aggregate(double value, uint32_t duration);

  // C2 from Welford's algorithm; used to compute the covariance of the measurements and timestamps weighted
  double get_c2() const { return this->c2_; }
  void set_c2(double c2) { this->c2_ = c2; }

  // Count of valid readings; i.e., not NaN, in the set of measurements
  size_t get_count() const { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }

  size_t get_duration() const { return this->duration_; }
  void set_duration(size_t duration) { this->duration_ = duration; }

  size_t get_duration_squared() const { return this->duration_squared_; }
  void set_duration_squared(size_t duration_squared) { this->duration_squared_ = duration_squared; }

  // M2 from Welford's algorithm; used to compute variance weighted
  double get_m2() const { return this->m2_; }
  void set_m2(double m2) { this->m2_ = m2; }

  // Maximum value of the set of measurements
  double get_max() const { return this->max_; }
  void set_max(double max) { this->max_ = max; }

  // average value in the set of measurements
  double get_mean() const { return this->mean_; }
  void set_mean(double mean) { this->mean_ = mean; }

  // minimum value in the set of measurements
  double get_min() const { return this->min_; }
  void set_min(double min) { this->min_ = min; }

  // M2 from Welford's algorithm for timestamps; used to compute variance of timestamps
  double get_timestamp_m2() const { return this->timestamp_m2_; }
  void set_timestamp_m2(double timestamp_m2) { this->timestamp_m2_ = timestamp_m2; }

  // Average timestamp for the aggregate; offset by timestamp_reference
  double get_timestamp_mean() const { return this->timestamp_mean_; }
  void set_timestamp_mean(double timestamp_mean) { this->timestamp_mean_ = timestamp_mean; }

  // The reference timestamp (in milliseconds) that all timestamps in the sum are offset by
  uint32_t get_timestamp_reference() const { return this->timestamp_reference_; }
  void set_timestamp_reference(uint32_t timestamp_reference) { this->timestamp_reference_ = timestamp_reference; }

  // Return the covariance of measurements and timestamps
  //  - applies Bessel's correction if GroupType is sample
  double compute_covariance(bool time_weighted, GroupType type) const;

  // Return the standard deviation of observations in aggregate
  //  - applies Bessel's correction if GroupType is sample
  double compute_std_dev(bool time_weighted, GroupType type) const;

  // Return the variance of observations in aggregate
  //  - applies Bessel's correction if GroupType is sample
  double compute_variance(bool time_weighted, GroupType type) const;

  // Return the slope of the line of best fit over the window
  double compute_trend() const;

  // Binary operation that combines two aggregates storing statistics from non-overlapping sets of measuremnts
  Aggregate combine_with(const Aggregate &b, bool time_weighted = false);

  // TEMPORARY means for debugging floating point precision operations
  double get_mean2() const { return this->mean2; }
  void set_mean2(double mean) { this->mean2 = mean; }
  double get_mean3() const { return this->mean3; }
  void set_mean3(double mean) { this->mean3 = mean; }
  double get_mean4() const { return this->mean4; }
  void set_mean4(double mean) { this->mean4 = mean; }

 protected:
  /*
   * Default values for the aggregates are the values for a null measurement or the empty set of measurements;
   * i.e., no measurement/observation at all
   */

  // Count of non-NaN measurements in the set of measurements
  size_t count_{0};

  // Sum of the durations between successive measurements in the set; kept as milliseconds
  size_t duration_{0};

  // Sum of the druations between successive measuremnts squred in the set; necessary for reliability weights
  size_t duration_squared_{0};

  // The reference timestamp for the timestamp mean values
  // e.g., if we have one raw timestamp at 5 ms and the reference is 5 ms, we store 0 ms in timestamp_mean
  uint32_t timestamp_reference_{0};

  // Quantity used in an extended Welford's algorithm for finding the covariance of the set of measurements and
  // timestamps
  double c2_{NAN};

  // Extrema of the set of measurements
  double max_{std::numeric_limits<double>::infinity() * (-1)};  // the supremum of the empty set is -infinity
  double min_{std::numeric_limits<double>::infinity()};         // the infimum of the empty set is +infinity

  // Quantity used in Welford's algorihtm for finding the variance of the set of measurements
  double m2_{NAN};

  // Average of the set of measurements
  double mean_{NAN};

  // Quantity used in Welford's algorithm for finding the variance of timestamps in the set of measurements
  double timestamp_m2_{NAN};

  double timestamp_mean_{NAN};

  // TEMPORARY
  double mean2{NAN};
  double mean3{NAN};
  double mean4{NAN};

  // Returns the appproriate denominator for the variance and covariance calculatioins
  //  - applies Bessel's correction if GroupType is sample and average type is not time weighted
  //  - uses reliability weights if GroupType is sample and average type is time weighted
  double denominator_(bool time_weighted, GroupType type) const;

  // Given two samples, normalize the timestamp means so that they are both in reference to the larger timestamp
  // returns the timestamp both sums are in reference to
  double normalize_timestamp_means_(double &a_mean, const uint32_t &a_timestamp_reference, const size_t &a_count,
                                    double &b_mean, const uint32_t &b_timestamp_reference, const size_t &b_count);
};

}  // namespace statistics
}  // namespace esphome
