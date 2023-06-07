/*
  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
*/

#pragma once

#include <limits>
#include "circular_queue.h"

namespace esphome {

namespace statistics {

// Aggregate summary statistics structure
struct Aggregate {
  float max;
  float min;

  double m2;  // used to find variance & standard deviation via Welford's algorithm

  float mean;

  size_t count;
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
  Aggregate query();

 protected:
  CircularQueue<Aggregate> queue_;

  // void debug_pointers_();

  // DABA Lite - Raw Indices for queue_; i.e., not offset by the head index
  size_t l_{0};
  size_t r_{0};
  size_t a_{0};
  size_t b_{0};

  // Summary statisitics for a null entry
  Aggregate identity_{
      std::numeric_limits<float>::infinity() * (-1),  // null max is -Infinity
      std::numeric_limits<float>::infinity(),         // null min is Infinity
      NAN,                                            // null M2 is NaN
      NAN,                                            // null mean is NaN
      0                                               // null count is 0
  };

  // DABA Lite - Running Totals
  Aggregate midSum_{this->identity_}, backSum_{this->identity_};

  // compute summary statistics for a single new value
  Aggregate lift_(float v);

  // combine summary statistics from two aggregates
  Aggregate combine_(Aggregate &a, Aggregate &b);

  // DABA Lite algorithm method
  void step_();
  // DABA Lite algorithm method
  void flip_();

  // DABA Lite algorithm methods
  inline bool is_front_empty_();
  inline bool is_delta_empty_();
  inline Aggregate get_back_();
  inline Aggregate get_alpha_();
  inline Aggregate get_delta_();
};

}  // namespace statistics
}  // namespace esphome
