/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
*/

#pragma once

#include <limits>
#include <algorithm>
#include "circular_queue.h"

namespace esphome {

namespace statistics {

// struct Statistics_Config {
//   static constexpr auto Include_t_m2 = true;
// };

class AggregateClass {
 public:
  size_t get_count() { return this->count_; }
  void set_count(size_t count) { this->count_ = count; }
  void combine_count(AggregateClass a, AggregateClass b) { this->count_ = a.get_count() + b.get_count(); }

  float get_max() { return this->max_; }
  void set_max(float max) { this->max_ = max; }
  void combine_max(AggregateClass a, AggregateClass b) { this->max_ = std::max(a.get_max(), b.get_max()); }

  float get_min() { return this->min_; }
  void set_min(float min) { this->min_ = min; }
  void combine_min(AggregateClass a, AggregateClass b) { this->min_ = std::min(a.get_min(), b.get_min()); }

  float get_mean() { return this->mean_; }
  void set_mean(float mean) { this->mean_ = mean; }
  void combine_mean(AggregateClass a, AggregateClass b) {
    this->mean_ = this->return_mean_(a.get_mean(), a.get_count(), b.get_mean(), b.get_count());
  }

  float get_m2() { return this->m2_; }
  void set_m2(float m2) { this->m2_ = m2; }
  void combine_m2(AggregateClass a, AggregateClass b) {
    this->m2_ = this->return_m2_(a.get_mean(), a.get_count(), a.get_m2(), b.get_mean(), b.get_count(), b.get_m2());
  }

  float get_t_mean() { return this->t_mean_; }
  void set_t_mean(float t_mean) { this->t_mean_ = t_mean; }
  void combine_t_mean(AggregateClass a, AggregateClass b) {
    this->t_mean_ = this->return_mean_(a.get_t_mean(), a.get_count(), b.get_t_mean(), b.get_count());
  }

  float get_t_m2() { return this->t_m2_; }
  void set_t_m2(float t_m2) { this->t_m2_ = t_m2; }
  void combine_t_m2(AggregateClass a, AggregateClass b) {
    this->t_m2_ =
        this->return_m2_(a.get_t_mean(), a.get_count(), a.get_t_m2(), b.get_t_mean(), b.get_count(), b.get_t_m2());
  }

  float get_c2() { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void combine_c2(AggregateClass a, AggregateClass b) {
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

class DABALite {
 public:
  void set_capacity(size_t window_size);
  size_t size();

  // insert a new value at end of circular queue and step DABA Lite algorithm
  void insert(float value);

  // remove value at start of circular queue and step DABA Lite algorithm
  void evict();

  // Return the summary statistics for all entries in the queue
  AggregateClass query();

  void enable_max() { this->include_max_ = true; }
  void enable_min() { this->include_min_ = true; }
  void enable_count() { this->include_count_ = true; }
  void enable_mean() { this->include_mean_ = true; }
  void enable_m2() { this->include_m2_ = true; }
  void enable_c2() { this->include_c2_ = true; }
  void enable_t_m2() { this->include_t_m2_ = true; }
  void enable_t_mean() { this->include_t_mean_ = true; }

 protected:
  std::vector<float> max_queue_{};
  std::vector<float> min_queue_{};
  std::vector<size_t> count_queue_{};
  std::vector<float> mean_queue_{};
  std::vector<float> m2_queue_{};
  std::vector<float> c2_queue_{};
  std::vector<float> t_mean_queue_{};
  std::vector<float> t_m2_queue_{};

  bool include_max_{false};
  bool include_min_{false};

  bool include_count_{false};
  bool include_mean_{false};

  bool include_m2_{false};

  bool include_c2_{false};
  bool include_t_m2_{false};

  bool include_t_mean_{false};

  void debug_pointers_();
  void emplace_(AggregateClass value, size_t index);

  size_t window_size_{0};

  // DABA Lite - Raw Indices for queue_; i.e., not offset by the head index
  CircularQueueIndex f_;
  CircularQueueIndex l_;
  CircularQueueIndex r_;
  CircularQueueIndex a_;
  CircularQueueIndex b_;
  CircularQueueIndex e_;

  AggregateClass identity_class_, midSum_, backSum_;  // assumes default values for a null entry

  // compute summary statistics for a single new value
  AggregateClass lift_(float v);

  // return summary statistics for a given index
  AggregateClass lower_(size_t index);

  // combine summary statistics from two aggregates
  AggregateClass combine_(AggregateClass &a, AggregateClass &b);

  // DABA Lite algorithm method
  void step_();
  // DABA Lite algorithm method
  void flip_();

  // DABA Lite algorithm methods
  inline bool is_front_empty_();
  inline bool is_delta_empty_();
  inline AggregateClass get_back_();
  inline AggregateClass get_alpha_();
  inline AggregateClass get_delta_();
};

}  // namespace statistics
}  // namespace esphome
