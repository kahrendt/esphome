/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
*/

#pragma once

#include <limits>
#include <algorithm>

namespace esphome {
namespace statistics {

class Aggregate {
 public:
  size_t get_count() { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }
  void combine_count(Aggregate a, Aggregate b) { this->count_ = a.get_count() + b.get_count(); }

  float get_max() { return this->max_; }
  void set_max(float max) { this->max_ = max; }
  void combine_max(Aggregate a, Aggregate b) { this->max_ = std::max(a.get_max(), b.get_max()); }

  float get_min() { return this->min_; }
  void set_min(float min) { this->min_ = min; }
  void combine_min(Aggregate a, Aggregate b) { this->min_ = std::min(a.get_min(), b.get_min()); }

  float get_mean() { return this->mean_; }
  void set_mean(float mean) { this->mean_ = mean; }
  void combine_mean(Aggregate a, Aggregate b) {
    this->mean_ = this->return_mean_(a.get_mean(), a.get_count(), b.get_mean(), b.get_count());
  }

  float get_m2() { return this->m2_; }
  void set_m2(float m2) { this->m2_ = m2; }
  void combine_m2(Aggregate a, Aggregate b) {
    this->m2_ = this->return_m2_(a.get_mean(), a.get_count(), a.get_m2(), b.get_mean(), b.get_count(), b.get_m2());
  }

  float get_t_mean() { return this->t_mean_; }
  void set_t_mean(float t_mean) { this->t_mean_ = t_mean; }
  void combine_t_mean(Aggregate a, Aggregate b) {
    this->t_mean_ = this->return_mean_(a.get_t_mean(), a.get_count(), b.get_t_mean(), b.get_count());
  }

  float get_t_m2() { return this->t_m2_; }
  void set_t_m2(float t_m2) { this->t_m2_ = t_m2; }
  void combine_t_m2(Aggregate a, Aggregate b) {
    this->t_m2_ =
        this->return_m2_(a.get_t_mean(), a.get_count(), a.get_t_m2(), b.get_t_mean(), b.get_count(), b.get_t_m2());
  }

  float get_c2() { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void combine_c2(Aggregate a, Aggregate b) {
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

  float get_variance() { return this->m2_ / (this->count_ - 1); }
  float get_std_dev() { return std::sqrt(this->get_variance()); }

  float get_covariance() { return this->c2_ / (this->count_ - 1); }
  float get_trend() { return this->c2_ / this->t_m2_; }

 protected:
  // default values represent the statistic for a null entry
  size_t count_{0};

  float max_{std::numeric_limits<float>::infinity() * (-1)};
  float min_{std::numeric_limits<float>::infinity()};

  float mean_{NAN};

  float m2_{NAN};

  float t_mean_{NAN};
  float t_m2_{NAN};
  float c2_{NAN};

  float return_mean_(float a_mean, size_t a_count, float b_mean, size_t b_count) {
    if (std::isnan(a_mean) && std::isnan(b_mean))
      return NAN;
    else if (std::isnan(a_mean))
      return b_mean;
    else if (std::isnan(b_mean))
      return a_mean;

    return (a_mean * static_cast<double>(a_count) + b_mean * static_cast<double>(b_count)) /
           (static_cast<double>(a_count + b_count));
  }

  float return_m2_(float a_mean, size_t a_count, float a_m2, float b_mean, size_t b_count, float b_m2) {
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
};

}  // namespace statistics
}  // namespace esphome
