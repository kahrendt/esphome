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

#include "aggregate.h"

#include "esphome/core/hal.h"  // necessary for millis()

#include <algorithm>  // necessary for std::min and std::max functions
#include <cmath>      // necessary for NaN

namespace esphome {
namespace statistics {

Aggregate::Aggregate() {}

Aggregate::Aggregate(float value) {
  if (!std::isnan(value)) {
    this->max_ = value;
    this->min_ = value;
    this->count_ = 1;
    this->mean_ = value;
    this->m2_ = 0.0;
    this->c2_ = 0.0;
    this->timestamp_m2_ = 0.0;
    this->timestamp_sum_ = 0;
    this->timestamp_reference_ = millis();
  }
}

Aggregate Aggregate::operator+(const Aggregate &b) {
  size_t a_count = this->get_count();
  size_t b_count = b.get_count();

  size_t cast_a_count = static_cast<double>(a_count);
  size_t cast_b_count = static_cast<double>(b_count);

  float a_min = this->get_min();
  float b_min = b.get_min();

  float a_max = this->get_max();
  float b_max = b.get_max();

  double a_mean = static_cast<double>(this->get_mean());
  double b_mean = static_cast<double>(b.get_mean());

  double a_m2 = static_cast<double>(this->get_m2());
  double b_m2 = static_cast<double>(b.get_m2());

  double a_c2 = static_cast<double>(this->get_c2());
  double b_c2 = static_cast<double>(b.get_c2());

  int32_t a_timestamp_sum = this->get_timestamp_sum();
  int32_t b_timestamp_sum = b.get_timestamp_sum();

  uint32_t a_timestamp_reference = this->get_timestamp_reference();
  uint32_t b_timestamp_reference = b.get_timestamp_reference();

  uint32_t a_timestamp_m2 = this->get_timestamp_m2();
  uint32_t b_timestamp_m2 = b.get_timestamp_m2();

  Aggregate combined;

  combined.count_ = a_count + b_count;

  combined.max_ = std::max(a_max, b_max);
  combined.min_ = std::min(a_min, b_min);

  combined.timestamp_reference_ = this->normalize_timestamp_sums_(a_timestamp_sum, a_timestamp_reference, a_count,
                                                                  b_timestamp_sum, b_timestamp_reference, b_count);
  combined.timestamp_sum_ = a_timestamp_sum + b_timestamp_sum;

  if (!a_count && !b_count) {
    combined.mean_ = NAN;
    combined.m2_ = NAN;
    combined.c2_ = NAN;
    combined.timestamp_m2_ = NAN;
  } else if (!a_count) {
    combined.mean_ = b_mean;
    combined.m2_ = b_m2;
    combined.c2_ = b_c2;
    combined.timestamp_m2_ = b_timestamp_m2;
  } else if (!b_count) {
    combined.mean_ = a_mean;
    combined.m2_ = a_m2;
    combined.c2_ = a_c2;
    combined.timestamp_m2_ = a_m2;
  } else {
    double delta = b_mean - a_mean;

    combined.mean_ = a_mean + delta * cast_b_count / (cast_a_count + cast_b_count);

    // compute M2 quantity for Welford's algorithm which can determine the variance
    combined.m2_ = a_m2 + b_m2 + delta * delta * cast_a_count * cast_b_count / (cast_a_count + cast_b_count);

    // use integer operations as much as possible to reduce floating point arithmetic when dealing with timestamps
    // store timestamp_delta as int64_t, as if we have a larger number of samples over a large time period, an int32_t
    // quickly overflows
    int64_t timestamp_delta = b_timestamp_sum * a_count - a_timestamp_sum * b_count;
    uint64_t timestamp_delta_squared = timestamp_delta * timestamp_delta;

    // compute C2 quantity for an extension of Welford's algorithm which can determine the covariance of timestamps and
    // measurements
    combined.c2_ =
        this->get_c2() + b.get_c2() + static_cast<double>(timestamp_delta) * delta / (cast_a_count + cast_b_count);

    size_t timestamp_m2_denominator = a_count * b_count * (a_count + b_count);

    combined.timestamp_m2_ =
        a_timestamp_m2 + b_timestamp_m2 +
        static_cast<double>(timestamp_delta_squared) / static_cast<double>(timestamp_m2_denominator);
  }

  return combined;
}

// Aggregate &Aggregate::operator+=(const Aggregate &rhs) {
//   Aggregate copy = *this;
//   return copy + *rhs;
// }

// Sample variance using Welford's algorithm (Bessel's correction is applied)
float Aggregate::compute_variance() const { return this->m2_ / (this->count_ - 1); }

// Sample standard deviation using Welford's algorithm (Bessel's correction is applied to the computed variance)
float Aggregate::compute_std_dev() const { return std::sqrt(this->compute_variance()); }

// Sample covariance using an extension of Welford's algorithm (Bessel's correction is applied)
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

// Given two samples a and b, normalize the timestamp sums so that they are both in reference to the larger timestamp
// returns the timestamp both sums are in reference to
uint32_t Aggregate::normalize_timestamp_sums_(int32_t &a_sum, const uint32_t &a_timestamp_reference,
                                              const size_t &a_count, int32_t &b_sum,
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
