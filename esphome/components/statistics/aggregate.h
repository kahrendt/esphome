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

  // average timestamp of measurements in the window
  float get_t_mean() const { return this->t_mean_; }
  void set_t_mean(float t_mean) { this->t_mean_ = t_mean; }
  void combine_t_mean(const Aggregate &a, const Aggregate &b);

  // C2 from Welford's algorithm; used to compute the covariance of the measurements and timestamps
  float get_c2() const { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void combine_c2(const Aggregate &a, const Aggregate &b);

  // M2 from Welford's algorithm for timestamps; used to compute variance of timestamps
  float get_t_m2() const { return this->t_m2_; }
  void set_t_m2(float t_m2) { this->t_m2_ = t_m2; }
  void combine_t_m2(const Aggregate &a, const Aggregate &b);

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
  size_t count_{0};

  float max_{std::numeric_limits<float>::infinity() * (-1)};  // the supremum of the empty set is -infinity
  float min_{std::numeric_limits<float>::infinity()};         // the infimum of the empty set is +infinity

  float mean_{NAN};

  float m2_{NAN};

  float t_mean_{NAN};
  float c2_{NAN};

  float t_m2_{NAN};

  // computes the mean using summary statistics from two non-overlapping samples (parallel algorithm)
  float combine_mean_(float a_mean, size_t a_count, float b_mean, size_t b_count) const;

  // computes M2 for Welford's algorithm using summary statistics from two non-overlapping samples (parallel algorithm)
  float combine_m2_(float a_mean, size_t a_count, float a_m2, float b_mean, size_t b_count, float b_m2) const;
};

}  // namespace statistics
}  // namespace esphome
