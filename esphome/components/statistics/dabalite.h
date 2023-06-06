/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
*/

#pragma once

#include <limits>
#include "circularqueue.h"

namespace esphome {

namespace statistics {

// partial summary statistics structure
struct Partial {
  float max;
  float min;

  double m2;  // used to find variance & standard deviation via Welford's algorithm

  float mean;

  size_t count;
};

class DABALite {
 public:
  // DABALite(size_t window_size);
  void set_window_size(size_t window_size);
  size_t size();

  // insert a new value at end of circular queue and step DABA Lite algorithm
  void insert(float value);

  // remove value at start of circular queue and step DABA Lite algorithm
  void evict();

  // Return the summary statistics for all entries in the queue
  Partial query();

 protected:
  // CircularQueue<Partial> *queue_{nullptr};
  CircularQueue<Partial> queue_;

  // DABA Lite - Indices
  size_t l_{0};
  size_t r_{0};
  size_t a_{0};
  size_t b_{0};

  // Summary statisitics for a null entry
  Partial identity_{std::numeric_limits<float>::infinity() * (-1), std::numeric_limits<float>::infinity(), 0.0, 0.0, 0};

  // DABA Lite - Running Totals
  Partial midSum_{this->identity_}, backSum_{this->identity_};

  // compute summary statistics for a single new value
  Partial lift_(float v);

  // combine summary statistics from two partial samples
  Partial combine_(Partial &a, Partial &b);

  // DABA Lite algorithm method
  void step_();
  // DABA Lite algorithm method
  void flip_();

  // DABA Lite algorithm methods
  inline bool is_front_empty_();
  inline bool is_delta_empty_();
  inline Partial get_back_();
  inline Partial get_alpha_();
  inline Partial get_delta_();
};

}  // namespace statistics
}  // namespace esphome
