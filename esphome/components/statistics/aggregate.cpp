#include "aggregate.h"

#include <algorithm>  // necessary for std::min and std::max functions
#include <cmath>      // necessary for NaN

namespace esphome {
namespace statistics {

Aggregate::Aggregate(double value, size_t duration, uint32_t timestamp) {
  if (!std::isnan(value)) {
    this->c2_ = 0.0;
    this->count_ = 1;
    this->m2_ = 0.0;
    this->max_ = value;
    this->mean_ = value;
    this->min_ = value;
    this->timestamp_m2_ = 0.0;
    this->timestamp_mean_ = 0.0;
    this->timestamp_reference_ = timestamp;
    this->duration_ = duration;
    this->duration_squared_ = duration * duration;
  }
}

Aggregate Aggregate::combine_with(const Aggregate &b, bool time_weighted) {
  // If either of the Aggregates are the identity, return the other Aggregate.
  if (b.get_count() == 0) {
    return *this;
  } else if (this->get_count() == 0) {
    return b;
  }

  Aggregate combined;

  combined.count_ = this->get_count() + b.get_count();
  combined.duration_ = this->get_duration() + b.get_duration();
  combined.duration_squared_ = this->get_duration_squared() + b.get_duration_squared();

  combined.max_ = std::max(this->get_max(), b.get_max());
  combined.min_ = std::min(this->get_min(), b.get_min());

  double a_weight, b_weight, combined_weight;

  double a_timestamp_mean = this->get_timestamp_mean();
  double b_timestamp_mean = b.get_timestamp_mean();

  combined.timestamp_reference_ =
      this->normalize_timestamp_means_(a_timestamp_mean, this->get_timestamp_reference(), this->get_count(),
                                       b_timestamp_mean, b.get_timestamp_reference(), b.get_count());

  // If the averages should be time weighted, then use measurement durations.
  // Otherwise, use the measurement counts as the weights.
  if (time_weighted) {
    a_weight = static_cast<double>(this->get_duration());
    b_weight = static_cast<double>(b.get_duration());
    combined_weight = static_cast<double>(combined.duration_);
  } else {
    a_weight = static_cast<double>(this->get_count());
    b_weight = static_cast<double>(b.get_count());
    combined_weight = static_cast<double>(combined.count_);
  }

  double delta = b.get_mean() - this->get_mean();
  double delta_prime = delta * b_weight / combined_weight;

  double timestamp_delta = b_timestamp_mean - a_timestamp_mean;
  double timestamp_delta_prime = timestamp_delta * b_weight / combined_weight;

  // Compute the mean and timestamp mean.
  if ((b.get_count() < combined.count_ / 4) || (this->get_count() < combined.count_ / 4)) {
    // If either count is signficantly smaller than the other, then use a variation of Welford's algorithm for speed.
    combined.mean_ = this->get_mean() + delta_prime;
    combined.timestamp_mean_ = a_timestamp_mean + timestamp_delta_prime;
  } else {
    // Otherwise, use a weighted average for numerical stability.
    combined.mean_ = this->get_mean() * (a_weight / combined_weight) + b.get_mean() * (b_weight / combined_weight);
    combined.timestamp_mean_ =
        a_timestamp_mean * (a_weight / combined_weight) + b_timestamp_mean * (b_weight / combined_weight);
  }

  // Compute M2 quantity for Welford's algorithm which determines the variance.
  combined.m2_ = this->get_m2() + b.get_m2() + a_weight * delta * delta_prime;
  combined.timestamp_m2_ =
      this->get_timestamp_m2() + b.get_timestamp_m2() + a_weight * timestamp_delta * timestamp_delta_prime;

  // Compute C2 quantity for a variation of Welford's algorithm which determines the covariance.
  combined.c2_ = this->get_c2() + b.get_c2() + a_weight * delta * timestamp_delta_prime;

  return combined;
}

double Aggregate::compute_covariance(bool time_weighted, GroupType type) const {
  if (this->count_ > 1)
    return this->c2_ / this->denominator_(time_weighted, type);

  return NAN;
}

double Aggregate::compute_std_dev(bool time_weighted, GroupType type) const {
  return std::sqrt(this->compute_variance(time_weighted, type));
}

double Aggregate::compute_trend() const {
  if (this->count_ > 1)
    return this->c2_ / this->timestamp_m2_;
  return NAN;
}

double Aggregate::compute_variance(bool time_weighted, GroupType type) const {
  if (this->count_ > 1)
    return this->m2_ / this->denominator_(time_weighted, type);

  return NAN;
}

//////////////////////
// Internal Methods //
//////////////////////

double Aggregate::denominator_(bool time_weighted, GroupType type) const {
  // Bessel's correction for non-time weighted samples (https://en.wikipedia.org/wiki/Bessel%27s_correction)
  double denominator = this->count_ - 1;

  if (!time_weighted) {
    if (type == POPULATION_GROUP_TYPE)
      denominator = static_cast<double>(this->count_);
  } else {
    if (type == SAMPLE_GROUP_TYPE) {
      // reliability weights: http://en.wikipedia.org/wiki/Weighted_arithmetic_mean#Weighted_sample_variance
      denominator = static_cast<double>(this->duration_) -
                    static_cast<double>(this->duration_squared_) / static_cast<double>(this->duration_);
    } else if (type == POPULATION_GROUP_TYPE)
      denominator = static_cast<double>(this->duration_);
  }

  return denominator;
}

double Aggregate::normalize_timestamp_means_(double &a_mean, const uint32_t &a_timestamp_reference,
                                             const size_t &a_count, double &b_mean,
                                             const uint32_t &b_timestamp_reference, const size_t &b_count) {
  if (a_count == 0) {
    // a is null, so b is always the more recent timestamp; no adjustments necessary
    return b_timestamp_reference;
  }
  if (b_count == 0) {
    // b is null, so a is always the more recent timestamp; no adjustments necessary
    return a_timestamp_reference;
  }

  // a and b both represent non-empty sets of measuremnts, so determine which timestamp is more recent
  // we test the sign bit of the difference to see if the subtraction rolls over
  //  - this assumes the references are not truly more than 2^31 ms apart, which is about 24.86 days
  //    (https://arduino.stackexchange.com/a/12591)
  if ((a_timestamp_reference - b_timestamp_reference) & 0x80000000) {
    // b is the more recent timestamp
    // normalize the a_sum using the b_timestamp

    uint32_t timestamp_delta = b_timestamp_reference - a_timestamp_reference;
    a_mean = a_mean - timestamp_delta;  // a_sum is now offset and normalized to b_timestamp_reference

    return b_timestamp_reference;  // both timestamps are in reference to b_timestamp_reference
  } else {
    // a is the more recent timestamp
    // normalize the b_sum using the a_timestamp

    uint32_t timestamp_delta = a_timestamp_reference - b_timestamp_reference;
    b_mean = b_mean - timestamp_delta;  // b_sum is now offset and normalized to a_timestamp_reference

    return a_timestamp_reference;
  }
}

}  // namespace statistics
}  // namespace esphome
