/*
  Aggregate methods for computing summary statistics
    - means are calculated in a manner that hopefully avoids catastrophic cancellation when using a large number of
      samples
    - variance and covariance aggregates are computed using a variation of Welford's alogorithm for parallel computing
*/

#include "aggregate.h"
#include <limits>
#include <algorithm>
#include <cmath>

namespace esphome {
namespace statistics {

void Aggregate::combine_count(Aggregate a, Aggregate b) { this->count_ = a.get_count() + b.get_count(); }

void Aggregate::combine_max(Aggregate a, Aggregate b) { this->max_ = std::max(a.get_max(), b.get_max()); }

void Aggregate::combine_min(Aggregate a, Aggregate b) { this->min_ = std::min(a.get_min(), b.get_min()); }

void Aggregate::combine_mean(Aggregate a, Aggregate b) {
  this->mean_ = this->combine_mean_(a.get_mean(), a.get_count(), b.get_mean(), b.get_count());
}

void Aggregate::combine_m2(Aggregate a, Aggregate b) {
  this->m2_ = this->combine_m2_(a.get_mean(), a.get_count(), a.get_m2(), b.get_mean(), b.get_count(), b.get_m2());
}

void Aggregate::combine_t_mean(Aggregate a, Aggregate b) {
  this->t_mean_ = this->combine_mean_(a.get_t_mean(), a.get_count(), b.get_t_mean(), b.get_count());
}

void Aggregate::combine_t_m2(Aggregate a, Aggregate b) {
  this->t_m2_ =
      this->combine_m2_(a.get_t_mean(), a.get_count(), a.get_t_m2(), b.get_t_mean(), b.get_count(), b.get_t_m2());
}

void Aggregate::combine_c2(Aggregate a, Aggregate b) {
  float a_c2 = a.get_c2();
  float b_c2 = b.get_c2();

  if (std::isnan(a_c2) && std::isnan(b_c2))
    this->c2_ = NAN;
  else if (std::isnan(a_c2))
    this->c2_ = b_c2;
  else if (std::isnan(b_c2))
    this->c2_ = a_c2;
  else {
    float a_mean = a.get_mean();
    double a_count = static_cast<double>(a.get_count());
    float a_t_mean = a.get_t_mean();

    float b_mean = b.get_mean();
    double b_count = static_cast<double>(b.get_count());
    float b_t_mean = b.get_t_mean();

    float delta = b_mean - a_mean;
    float t_delta = b_t_mean - a_t_mean;

    // compute C2 for an extension of Welford's algorithm to compute the covariance of t (timestamps) and y (sensor
    // measurements)
    this->c2_ = a_c2 + b_c2 + t_delta * delta * a_count * b_count / (a_count + b_count);
  }
}

float Aggregate::compute_variance() { return this->m2_ / (this->count_ - 1); }
float Aggregate::compute_std_dev() { return std::sqrt(this->compute_variance()); }

float Aggregate::compute_covariance() { return this->c2_ / (this->count_ - 1); }
float Aggregate::compute_trend() { return this->c2_ / this->t_m2_; }

float Aggregate::combine_mean_(float a_mean, size_t a_count, float b_mean, size_t b_count) {
  if (std::isnan(a_mean) && std::isnan(b_mean))
    return NAN;
  else if (std::isnan(a_mean))
    return b_mean;
  else if (std::isnan(b_mean))
    return a_mean;

  return (a_mean * static_cast<double>(a_count) + b_mean * static_cast<double>(b_count)) /
         (static_cast<double>(a_count + b_count));
}

float Aggregate::combine_m2_(float a_mean, size_t a_count, float a_m2, float b_mean, size_t b_count, float b_m2) {
  if (std::isnan(a_m2) && std::isnan(b_m2))
    return NAN;
  else if (std::isnan(a_m2))
    return b_m2;
  else if (std::isnan(b_m2))
    return a_m2;

  float delta = b_mean - a_mean;
  return a_m2 + b_m2 +
         delta * delta * (static_cast<double>(a_count)) * (static_cast<double>(b_count)) /
             ((static_cast<double>(a_count)) + (static_cast<double>(b_count)));
}

}  // namespace statistics
}  // namespace esphome
