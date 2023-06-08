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
  void set_count() { this->count_ = 0; }  // identity
  void combine_count(AggregateClass a, AggregateClass b) { this->count_ = a.get_count() + b.get_count(); }

  float get_max() { return this->max_; }
  void set_max(float max) { this->max_ = max; }
  void set_max() { this->max_ = std::numeric_limits<float>::infinity() * (-1); }  // identity
  void combine_max(AggregateClass a, AggregateClass b) { this->max_ = std::max(a.get_max(), b.get_max()); }

  float get_min() { return this->min_; }
  void set_min(float min) { this->min_ = min; }
  void set_min() { this->min_ = std::numeric_limits<float>::infinity(); }  // identity
  void combine_min(AggregateClass a, AggregateClass b) { this->min_ = std::min(a.get_min(), b.get_min()); }

  float get_mean() { return this->mean_; }
  void set_mean(float mean) { this->mean_ = mean; }
  void set_mean() { this->mean_ = NAN; }  // identity
  void combine_mean(AggregateClass a, AggregateClass b) {
    this->mean_ = this->return_mean_(a.get_mean(), a.get_count(), b.get_mean(), b.get_count());
  }

  float get_m2() { return this->m2_; }
  void set_m2(float m2) { this->m2_ = m2; }
  void set_m2() { this->m2_ = NAN; }  // identity
  void combine_m2(AggregateClass a, AggregateClass b) {
    this->m2_ = this->return_m2_(a.get_mean(), a.get_count(), a.get_m2(), b.get_mean(), b.get_count(), b.get_m2());
  }

  float get_t_mean() { return this->t_mean_; }
  void set_t_mean(float t_mean) { this->t_mean_ = t_mean; }
  void set_t_mean() { this->t_mean_ = NAN; }  // identity
  void combine_t_mean(AggregateClass a, AggregateClass b) {
    this->t_mean_ = this->return_mean_(a.get_t_mean(), a.get_count(), b.get_t_mean(), b.get_count());
  }

  float get_t_m2() { return this->t_m2_; }
  void set_t_m2(float t_m2) { this->t_m2_ = t_m2; }
  void set_t_m2() { this->t_m2_ = NAN; }  // identity
  void combine_t_m2(AggregateClass a, AggregateClass b) {
    this->t_m2_ =
        this->return_m2_(a.get_t_mean(), a.get_count(), a.get_t_m2(), b.get_t_mean(), b.get_count(), b.get_t_m2());
  }

  float get_c2() { return this->c2_; }
  void set_c2(float c2) { this->c2_ = c2; }
  void set_c2() { this->c2_ = NAN; }  // identity
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
  size_t count_;

  float max_;
  float min_;

  float mean_;

  float m2_;

  float t_mean_;
  float t_m2_;
  float c2_;

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

// // Aggregate summary statistics structure
// struct Aggregate {
//   float max;
//   float min;

//   double m2;  // used to find variance & standard deviation via Welford's algorithm

//   float mean;

//   size_t count;

//   // timestamp mean and M2
//   double t_mean;
//   double t_m2;

//   // used to find covariance
//   double c2;
// };

class DABALite {
 public:
  void set_identity();

  void set_capacity(size_t window_size);
  size_t size();

  // insert a new value at end of circular queue and step DABA Lite algorithm
  void insert(float value);

  // remove value at start of circular queue and step DABA Lite algorithm
  void evict();

  // Return the summary statistics for all entries in the queue
  AggregateClass query();
  // // Return the summary statistics for all entries in the queue
  // Aggregate query();

  void enable_max() { this->include_max_ = true; }
  void enable_min() { this->include_min_ = true; }
  void enable_count() { this->include_count_ = true; }
  void enable_mean() { this->include_mean_ = true; }
  void enable_m2() { this->include_m2_ = true; }
  void enable_c2() { this->include_c2_ = true; }
  void enable_t_m2() { this->include_t_m2_ = true; }

 protected:
  // CircularQueue<Aggregate> queue_;
  CircularQueue<AggregateClass> queue_class_;

  bool include_max_{false};
  bool include_min_{false};

  bool include_count_{false};
  bool include_mean_{false};

  bool include_m2_{false};

  bool include_c2_{false};
  bool include_t_m2_{false};

  // void debug_pointers_();

  // DABA Lite - Raw Indices for queue_; i.e., not offset by the head index
  size_t l_{0};
  size_t r_{0};
  size_t a_{0};
  size_t b_{0};

  AggregateClass identity_class_, midSum_, backSum_;

  // Summary statisitics for a null entry
  // Aggregate identity_{
  //     std::numeric_limits<float>::infinity() * (-1),  // null max is -Infinity
  //     std::numeric_limits<float>::infinity(),         // null min is Infinity
  //     NAN,                                            // null M2 is NaN
  //     NAN,                                            // null mean is NaN
  //     0,                                              // null count is 0
  //     NAN,                                            // null t_mean is NaN
  //     NAN,                                            // null t_m2 is NaN
  //     NAN                                             // null C2 is NaN
  // };

  // DABA Lite - Running Totals
  // Aggregate midSum_{this->identity_}, backSum_{this->identity_};

  // compute summary statistics for a single new value
  AggregateClass lift_(float v);
  // Aggregate lift_(float v);

  // combine summary statistics from two aggregates
  AggregateClass combine_(AggregateClass &a, AggregateClass &b);
  // Aggregate combine_(Aggregate &a, Aggregate &b);

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
  // inline Aggregate get_back_();
  // inline Aggregate get_alpha_();
  // inline Aggregate get_delta_();
};

}  // namespace statistics
}  // namespace esphome
