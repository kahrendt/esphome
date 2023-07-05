/*

 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"

namespace esphome {
namespace statistics {

struct EnabledAggregatesConfiguration {
  bool c2{false};
  bool count{false};
  bool duration{false};
  bool duration_squared{false};
  bool m2{false};
  bool max{false};
  bool mean{false};
  bool min{false};
  bool timestamp_m2{false};
  bool timestamp_mean{false};
  bool timestamp_reference{false};
};

class AggregateQueue {
 public:
  void enable_time_weighted() { this->time_weighted_ = true; }
  size_t size() const { return this->size_; };

  virtual bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) = 0;
  virtual void clear() = 0;
  virtual void insert(Aggregate value) = 0;
  virtual void evict() = 0;
  virtual Aggregate compute_current_aggregate() = 0;

  void emplace(const Aggregate &value, size_t index);
  Aggregate lower(size_t index);

  bool allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

 protected:
  bool time_weighted_{false};

  size_t size_{0};

  size_t *count_queue_{nullptr};
  size_t *duration_squared_queue_{nullptr};

  uint32_t *duration_queue_{nullptr};
  uint32_t *timestamp_reference_queue_{nullptr};

  float *max_queue_{nullptr};
  float *mean_queue_{nullptr};
  float *min_queue_{nullptr};

  double *c2_queue_{nullptr};
  double *m2_queue_{nullptr};
  double *timestamp_m2_queue_{nullptr};
  double *timestamp_mean_queue_{nullptr};

  float *mean2_queue_{nullptr};
  float *mean3_queue_{nullptr};
  float *mean4_queue_{nullptr};
};

}  // namespace statistics
}  // namespace esphome
