/*
  Aggregate methods for computing summary statistics
    - means are calculated in a manner that hopefully avoids catastrophic cancellation when using a large number of
      samples
    - variance and covariance aggregates are computed using a variation of Welford's alogorithm for parallel computing

  Implemented by Kevin Ahrendt, June 2023
*/

#include "aggregate.h"

#include <algorithm>  // necessary for std::min and std::max functions
#include <cmath>      // necessary for NaN

namespace esphome {
namespace statistics {

void Aggregate::combine_count(const Aggregate &a, const Aggregate &b) { this->count_ = a.get_count() + b.get_count(); }

void Aggregate::combine_max(const Aggregate &a, const Aggregate &b) { this->max_ = std::max(a.get_max(), b.get_max()); }

void Aggregate::combine_min(const Aggregate &a, const Aggregate &b) { this->min_ = std::min(a.get_min(), b.get_min()); }

// computes the mean using summary statistics from two disjoint samples (parallel algorithm)
void Aggregate::combine_mean(const Aggregate &a, const Aggregate &b) {
  if (std::isnan(a.get_mean()) && std::isnan(b.get_mean())) {
    this->mean_ = NAN;
  } else if (std::isnan(a.get_mean())) {
    this->mean_ = b.get_mean();
  } else if (std::isnan(b.get_mean())) {
    this->mean_ = a.get_mean();
  } else {
    this->mean_ =
        (a.get_mean() * static_cast<double>(a.get_count()) + b.get_mean() * static_cast<double>(b.get_count())) /
        (static_cast<double>(a.get_count() + b.get_count()));
  }
}

// computes M2 for Welford's algorithm using summary statistics from two non-overlapping samples (parallel algorithm)
void Aggregate::combine_m2(const Aggregate &a, const Aggregate &b) {
  if (std::isnan(a.get_m2()) && std::isnan(b.get_m2())) {
    this->m2_ = NAN;
  } else if (std::isnan(a.get_m2())) {
    this->m2_ = b.get_m2();
  } else if (std::isnan(b.get_m2())) {
    this->m2_ = a.get_m2();
  } else {
    double delta = b.get_mean() - a.get_mean();
    this->m2_ = a.get_m2() + b.get_m2() +
                delta * delta * (static_cast<double>(a.get_count())) * (static_cast<double>(b.get_count())) /
                    ((static_cast<double>(a.get_count())) + (static_cast<double>(b.get_count())));
  }
}

void Aggregate::combine_timestamp_sum(const Aggregate &a, const Aggregate &b) {
  __int32_t a_sum = a.get_timestamp_sum();
  __int32_t b_sum = b.get_timestamp_sum();

  this->timestamp_reference_ = this->normalize_timestamp_sums_(a_sum, a.get_timestamp_reference(), a.get_count(), b_sum,
                                                               b.get_timestamp_reference(), b.get_count());
  this->timestamp_sum_ = a_sum + b_sum;
}

// parallel algorithm for combining two C2 values from two non-overlapping samples
void Aggregate::combine_c2(const Aggregate &a, const Aggregate &b) {
  float a_c2 = a.get_c2();
  float b_c2 = b.get_c2();

  __int32_t a_timestamp_sum = a.get_timestamp_sum();
  __int32_t b_timestamp_sum = b.get_timestamp_sum();
  uint32_t a_timestamp_reference = a.get_timestamp_reference();
  uint32_t b_timestamp_reference = b.get_timestamp_reference();
  size_t a_count = a.get_count();
  size_t b_count = b.get_count();

  if (std::isnan(a_c2) && std::isnan(b_c2)) {
    this->c2_ = NAN;
  } else if (std::isnan(a_c2)) {
    this->c2_ = b_c2;
    this->timestamp_reference_ = b_timestamp_reference;
    this->timestamp_sum_ = b_timestamp_sum;
  } else if (std::isnan(b_c2)) {
    this->c2_ = a_c2;
    this->timestamp_reference_ = a_timestamp_reference;
    this->timestamp_sum_ = a_timestamp_sum;
  } else {
    // noramlize a_sum and b_sum so they are referenced from the same timestamp
    this->normalize_timestamp_sums_(a_timestamp_sum, a_timestamp_reference, a_count, b_timestamp_sum,
                                    b_timestamp_reference, b_count);

    // use interger operations as much as possible to reduce floating point arithmetic
    // store as int64_t, as if we have a larger number of samples over a large time period, an int32_t quickly overflows
    __int64_t timestamp_sum_difference = b_timestamp_sum * a_count - a_timestamp_sum * b_count;
    size_t total_count_second_converted = (a_count + b_count) * 1000;  // 1000 ms per 1 second

    float delta = b.get_mean() - a.get_mean();

    // compute C2 for an extension of Welford's algorithm to compute the covariance of timestamps and measurements
    this->c2_ = a_c2 + b_c2 + timestamp_sum_difference * delta / static_cast<double>(total_count_second_converted);
  }
}

void Aggregate::combine_timestamp_m2(const Aggregate &a, const Aggregate &b) {
  if (std::isnan(a.get_timestamp_m2()) && std::isnan(b.get_timestamp_m2())) {
    this->timestamp_m2_ = NAN;
  } else if (std::isnan(a.get_timestamp_m2())) {
    this->timestamp_m2_ = b.get_timestamp_m2();
  } else if (std::isnan(b.get_timestamp_m2())) {
    this->timestamp_m2_ = a.get_timestamp_m2();
  } else {
    __int32_t a_sum = a.get_timestamp_sum();
    __int32_t b_sum = b.get_timestamp_sum();

    // noramlize a_sum and b_sum so they are referenced from the same timestamp
    this->normalize_timestamp_sums_(a_sum, a.get_timestamp_reference(), a.get_count(), b_sum,
                                    b.get_timestamp_reference(), b.get_count());

    // use interger operations as much as possible to reduce floating point arithmetic
    // store as int64_t, as if we have a larger number of samples over a large time period, an int32_t quickly overflows
    __int64_t delta = b_sum * a.get_count() - a_sum * b.get_count();
    uint64_t delta_squared = delta * delta;

    size_t denominator = 1000 * 1000 * a.get_count() * b.get_count() * (a.get_count() + b.get_count());

    // compute M2 for Welford's algorithm to find the variance
    this->timestamp_m2_ = a.get_timestamp_m2() + b.get_timestamp_m2() +
                          static_cast<double>(delta_squared) / static_cast<double>(denominator);
  }
}

// Sample variance using Welford's algorithm (Bessel's correction is applied)
float Aggregate::compute_variance() const { return this->m2_ / (this->count_ - 1); }

// Sample standard deviation using Welford's algorithm (Bessel's correction is applied)
float Aggregate::compute_std_dev() const { return std::sqrt(this->compute_variance()); }

// Sample covariance using a variation of Welford's algorithm (Bessel's correction is applied)
float Aggregate::compute_covariance() const {
  if (this->count_ > 1)
    return this->c2_ / (this->count_ - 1);
  return NAN;
}

// Slope of the line of best fit over sliding window
float Aggregate::compute_trend() const {
  if (this->count_ > 1)
    return this->c2_ / this->timestamp_m2_;
  return NAN;
}

// given two samples, normalize the timestamp sums so that they are both in reference to the more recent timestamp
// returns the timestamp both sums are in reference to
uint32_t Aggregate::normalize_timestamp_sums(int32_t &a_sum, const uint32_t &a_timestamp_reference,
                                              const size_t &a_count, __int32_t &b_sum,
                                              const uint32_t &b_timestamp_reference, const size_t &b_count) {
  if (a_count == 0) {
    // a is null, so b is always the more recent timestamp; no adjustments necessary
    return b_timestamp_reference;
  }
  if (b_count == 0) {
    // b is null, so a is always the more recent timestamp; no adjustments necessary
    return a_timestamp_reference;
  }

  // a and b both represent actual measurements, so determine which timestamp is more recent
  // we test the sign bit of the difference to see if the subtraction rolls over
  //  - this assumes the references are not truly more than 2^31 ms apart, which is about 24.86 days
  //    (https://arduino.stackexchange.com/a/12591)
  if ((a_timestamp_reference - b_timestamp_reference) & 0x80000000) {
    // b is the more recent timestamp
    // normalize the a_sum using the b_timestamp

    uint32_t timestamp_delta = b_timestamp_reference - a_timestamp_reference;
    a_sum = a_sum - timestamp_delta * a_count;  // a_sum is now offset and normalized to b_timestamp_reference

    return b_timestamp_reference;  // both timestamps are in reference to b_timestamp_reference
  } else {
    // a is the more recent timestamp
    // normalize the b_sum using the a_timestamp

    uint32_t timestamp_delta = a_timestamp_reference - b_timestamp_reference;
    b_sum = b_sum - timestamp_delta * b_count;  // b_sum is now offset and normalized to a_timestamp_reference

    return a_timestamp_reference;
  }
}

}  // namespace statistics
}  // namespace esphome
