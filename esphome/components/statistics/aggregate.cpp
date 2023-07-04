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

#include "esphome/core/hal.h"      // necessary for millis()
#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator
#include "esphome/core/log.h"

#include <algorithm>  // necessary for std::min and std::max functions
#include <cmath>      // necessary for NaN

namespace esphome {
namespace statistics {

Aggregate::Aggregate(double value, uint32_t duration) {
  if (!std::isnan(value)) {
    this->max_ = value;
    this->min_ = value;
    this->count_ = 1;
    this->mean_ = value;
    this->m2_ = 0.0;
    this->c2_ = 0.0;
    this->timestamp_m2_ = 0.0;
    this->timestamp_reference_ = millis();
    this->timestamp_mean_ = 0.0;

    this->mean2 = value;
    this->mean3 = value;
    this->mean4 = value;
  }
  this->duration_ = duration;  // even if reading is NaN, still count the time that has passed
  this->duration_squared_ = duration * duration;
}

Aggregate Aggregate::combine_with(const Aggregate &b, bool time_weighted) {
  size_t a_count = this->get_count();
  size_t b_count = b.get_count();

  uint32_t a_duration = this->get_duration();
  uint32_t b_duration = b.get_duration();

  double a_min = this->get_min();
  double b_min = b.get_min();

  double a_max = this->get_max();
  double b_max = b.get_max();

  double a_mean = static_cast<double>(this->get_mean());
  double b_mean = static_cast<double>(b.get_mean());

  double a_m2 = static_cast<double>(this->get_m2());
  double b_m2 = static_cast<double>(b.get_m2());

  double a_c2 = static_cast<double>(this->get_c2());
  double b_c2 = static_cast<double>(b.get_c2());

  uint32_t a_timestamp_reference = this->get_timestamp_reference();
  uint32_t b_timestamp_reference = b.get_timestamp_reference();

  double a_timestamp_m2 = this->get_timestamp_m2();
  double b_timestamp_m2 = b.get_timestamp_m2();

  double a_timestamp_mean = this->get_timestamp_mean();
  double b_timestamp_mean = b.get_timestamp_mean();

  Aggregate combined;

  combined.count_ = a_count + b_count;
  combined.duration_ = a_duration + b_duration;
  combined.duration_squared_ = this->get_duration_squared() + b.get_duration_squared();

  double a_weight, b_weight, combined_weight;

  if (time_weighted) {
    a_weight = static_cast<double>(a_duration);
    b_weight = static_cast<double>(b_duration);
    combined_weight = static_cast<double>(combined.duration_);
  } else {
    a_weight = static_cast<double>(a_count);
    b_weight = static_cast<double>(b_count);
    combined_weight = static_cast<double>(combined.count_);
  }

  combined.max_ = std::max(a_max, b_max);
  combined.min_ = std::min(a_min, b_min);

  combined.timestamp_reference_ = this->normalize_timestamp_means_(a_timestamp_mean, a_timestamp_reference, a_count,
                                                                   b_timestamp_mean, b_timestamp_reference, b_count);

  if ((a_count == 0) && (b_count == 0)) {
    combined.mean_ = NAN;
    combined.m2_ = NAN;
    combined.c2_ = NAN;
    combined.timestamp_m2_ = NAN;
    combined.timestamp_mean_ = NAN;
  } else if (a_count == 0) {
    combined.mean_ = b_mean;
    combined.m2_ = b_m2;
    combined.c2_ = b_c2;
    combined.timestamp_m2_ = b_timestamp_m2;
    combined.timestamp_mean_ = b_timestamp_mean;

    combined.mean2 = b.get_mean2();
    combined.mean3 = b.get_mean3();
    combined.mean4 = b.get_mean4();
  } else if (b_count == 0) {
    combined.mean_ = a_mean;
    combined.m2_ = a_m2;
    combined.c2_ = a_c2;
    combined.timestamp_m2_ = a_timestamp_m2;
    combined.timestamp_mean_ = a_timestamp_mean;

    combined.mean2 = this->get_mean2();
    combined.mean3 = this->get_mean3();
    combined.mean4 = this->get_mean4();
  } else {
    double delta = b_mean - a_mean;
    double delta_prime = delta * b_weight / combined_weight;

    /*
     * Simple weighted combination of the mean; can potentially avoid issues if a_weight = b_weight
     *... but may pick up issues if a_weight or b_weight is small... try using accumlators?
     */
    double a_mean_weighted = a_mean * a_weight / combined_weight;
    double b_mean_weighted = b_mean * b_weight / combined_weight;

    combined.mean_ = a_mean_weighted + b_mean_weighted;

    /*
     * Basic Welford's version of mean.. may be unstable if a_weight = b_weight (see
     * https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Parallel_algorithm)
     */
    double delta2 = b.get_mean2() - this->get_mean2();
    double delta2_prime = delta2 * b_weight / combined_weight;
    combined.mean2 = this->get_mean2() + delta2_prime;
    // combined.mean_ = a_mean + delta_prime;

    // compute M2 quantity for Welford's algorithm which can determine the variance
    combined.m2_ = a_m2 + b_m2 + a_weight * delta * delta_prime;
    // combined.m2_ = a_m2 + b_m2 + delta * delta * a_weight * b_weight / (combined_weight);

    double timestamp_delta = b_timestamp_mean - a_timestamp_mean;
    double timestamp_delta_prime = timestamp_delta * b_weight / combined_weight;

    combined.timestamp_mean_ =
        a_timestamp_mean * (a_weight / combined_weight) + b_timestamp_mean * (b_weight / combined_weight);
    // combined.timestamp_mean_ = a_timestamp_mean + timestamp_delta_prime;

    combined.timestamp_m2_ = a_timestamp_m2 + b_timestamp_m2 + a_weight * timestamp_delta * timestamp_delta_prime;

    combined.c2_ = a_c2 + b_c2 + delta * timestamp_delta * (a_weight * b_weight / (combined_weight));
    // combined.c2_ = a_c2 + b_c2 + a_weight * delta * timestamp_delta_prime;
  }

  return combined;
}

// Sample variance using Welford's algorithm (Bessel's correction is applied)
double Aggregate::compute_variance(bool time_weighted, GroupType type) const {
  if (this->count_ > 1)
    return this->m2_ / this->denominator_(time_weighted, type);

  return NAN;
}

// Sample standard deviation using Welford's algorithm (Bessel's correction is applied to the computed variance)
double Aggregate::compute_std_dev(bool time_weighted, GroupType type) const {
  return std::sqrt(this->compute_variance(time_weighted, type));
}

// Sample covariance using an extension of Welford's algorithm (Bessel's correction is applied)
double Aggregate::compute_covariance(bool time_weighted, GroupType type) const {
  if (this->count_ > 1)
    return this->c2_ / this->denominator_(time_weighted, type);

  return NAN;
}

// Slope of the line of best fit over sliding window
double Aggregate::compute_trend() const {
  if (this->count_ > 1)
    return this->c2_ / this->timestamp_m2_;
  return NAN;
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

template<typename T> void AggregateQueue<T>::emplace(const Aggregate &value, size_t index) {
  if (this->max_queue_ != nullptr)
    this->max_queue_[index] = value.get_max();
  if (this->min_queue_ != nullptr)
    this->min_queue_[index] = value.get_min();
  if (this->count_queue_ != nullptr)
    this->count_queue_[index] = value.get_count();
  if (this->duration_queue_ != nullptr)
    this->duration_queue_[index] = value.get_duration();
  if (this->duration_squared_queue_ != nullptr)
    this->duration_squared_queue_[index] = value.get_duration_squared();
  if (this->mean_queue_ != nullptr) {
    this->mean_queue_[index] = value.get_mean();

    this->mean2_queue_[index] = value.get_mean2();
    this->mean3_queue_[index] = value.get_mean3();
    this->mean4_queue_[index] = value.get_mean4();
  }
  if (this->m2_queue_ != nullptr)
    this->m2_queue_[index] = value.get_m2();
  if (this->c2_queue_ != nullptr)
    this->c2_queue_[index] = value.get_c2();
  if (this->timestamp_mean_queue_ != nullptr)
    this->timestamp_mean_queue_[index] = value.get_timestamp_mean();
  if (this->timestamp_m2_queue_ != nullptr)
    this->timestamp_m2_queue_[index] = value.get_timestamp_m2();
  if (this->timestamp_reference_queue_ != nullptr)
    this->timestamp_reference_queue_[index] = value.get_timestamp_reference();
}

template<typename T> Aggregate AggregateQueue<T>::lower(size_t index) {
  Aggregate aggregate = Aggregate();

  if (this->max_queue_ != nullptr)
    aggregate.set_max(this->max_queue_[index]);
  if (this->min_queue_ != nullptr)
    aggregate.set_min(this->min_queue_[index]);
  if (this->count_queue_ != nullptr)
    aggregate.set_count(this->count_queue_[index]);
  if (this->duration_queue_ != nullptr)
    aggregate.set_duration(this->duration_queue_[index]);
  if (this->duration_squared_queue_ != nullptr)
    aggregate.set_duration_squared(this->duration_squared_queue_[index]);
  if (this->mean_queue_ != nullptr) {
    aggregate.set_mean(this->mean_queue_[index]);

    aggregate.set_mean2(this->mean2_queue_[index]);
    aggregate.set_mean3(this->mean3_queue_[index]);
    aggregate.set_mean4(this->mean4_queue_[index]);
  }
  if (this->m2_queue_ != nullptr)
    aggregate.set_m2(this->m2_queue_[index]);
  if (this->c2_queue_ != nullptr)
    aggregate.set_c2(this->c2_queue_[index]);
  if (this->timestamp_m2_queue_ != nullptr)
    aggregate.set_timestamp_m2(this->timestamp_m2_queue_[index]);
  if (this->timestamp_mean_queue_ != nullptr)
    aggregate.set_timestamp_mean(this->timestamp_mean_queue_[index]);
  if (this->timestamp_reference_queue_ != nullptr)
    aggregate.set_timestamp_reference(this->timestamp_reference_queue_[index]);

  return aggregate;
}

template<typename T> bool AggregateQueue<T>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config) {
  // Mimics ESPHome's rp2040_pio_led_strip component's buf_ code (accessed June 2023)
  ExternalRAMAllocator<T> decimal_allocator(ExternalRAMAllocator<T>::ALLOW_FAILURE);
  ExternalRAMAllocator<size_t> size_t_allocator(ExternalRAMAllocator<size_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<uint32_t> uint32_t_allocator(ExternalRAMAllocator<uint32_t>::ALLOW_FAILURE);

  if (config.max) {
    this->max_queue_ = decimal_allocator.allocate(capacity);
    if (this->max_queue_ == nullptr) {
      return false;
    }
  }

  if (config.min) {
    this->min_queue_ = decimal_allocator.allocate(capacity);
    if (this->min_queue_ == nullptr) {
      return false;
    }
  }

  if (config.count) {
    this->count_queue_ = size_t_allocator.allocate(capacity);
    if (this->count_queue_ == nullptr) {
      return false;
    }
  }

  if (config.duration) {
    this->duration_queue_ = uint32_t_allocator.allocate(capacity);  // uint32_t_allocator.allocate(capacity);
    if (this->duration_queue_ == nullptr) {
      return false;
    }
  }

  if (config.duration_squared) {
    this->duration_squared_queue_ = size_t_allocator.allocate(capacity);

    if (this->duration_squared_queue_ == nullptr) {
      return false;
    }
  }

  if (config.mean) {
    this->mean_queue_ = decimal_allocator.allocate(capacity);

    this->mean2_queue_ = decimal_allocator.allocate(capacity);
    this->mean3_queue_ = decimal_allocator.allocate(capacity);
    this->mean4_queue_ = decimal_allocator.allocate(capacity);

    if (this->mean_queue_ == nullptr) {
      return false;
    }
  }

  if (config.m2) {
    this->m2_queue_ = decimal_allocator.allocate(capacity);
    if (this->m2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.c2) {
    this->c2_queue_ = decimal_allocator.allocate(capacity);
    if (this->c2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_m2) {
    this->timestamp_m2_queue_ = decimal_allocator.allocate(capacity);
    if (this->timestamp_m2_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_mean) {
    this->timestamp_mean_queue_ = decimal_allocator.allocate(capacity);
    if (this->timestamp_mean_queue_ == nullptr) {
      return false;
    }
  }

  if (config.timestamp_reference) {
    this->timestamp_reference_queue_ = uint32_t_allocator.allocate(capacity);
    if (this->timestamp_reference_queue_ == nullptr) {
      return false;
    }
  }

  return true;
}

// avoids linking errors (https://isocpp.org/wiki/faq/templates)

template void AggregateQueue<float>::emplace(const Aggregate &value, size_t index);
template Aggregate AggregateQueue<float>::lower(size_t index);
template bool AggregateQueue<float>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

template void AggregateQueue<double>::emplace(const Aggregate &value, size_t index);
template Aggregate AggregateQueue<double>::lower(size_t index);
template bool AggregateQueue<double>::allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

}  // namespace statistics
}  // namespace esphome
