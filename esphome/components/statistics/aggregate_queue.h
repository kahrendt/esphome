/*

 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"

namespace esphome {
namespace statistics {

struct EnabledAggregatesConfiguration {
  bool count{false};
  bool duration{false};
  bool duration_squared{false};
  bool max{false};
  bool min{false};
  bool mean{false};
  bool m2{false};
  bool timestamp_mean{false};
  bool c2{false};
  bool timestamp_m2{false};
  bool timestamp_reference{false};
};

template<typename T> class AggregateQueue {
 public:
  virtual void enable_time_weighted();
  virtual bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config);
  virtual void clear();
  virtual size_t size() const;
  virtual void insert(T value, uint32_t duration);
  virtual void insert(Aggregate value);
  virtual void evict();
  virtual Aggregate compute_current_aggregate();

  void emplace(const Aggregate &value, size_t index);
  Aggregate lower(size_t index);

  bool allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

 protected:
  size_t *count_queue_{nullptr};
  size_t *duration_squared_queue_{nullptr};
  uint32_t *duration_queue_{nullptr};
  T *max_queue_{nullptr};
  T *min_queue_{nullptr};
  T *mean_queue_{nullptr};
  T *m2_queue_{nullptr};
  T *c2_queue_{nullptr};
  T *timestamp_m2_queue_{nullptr};
  T *timestamp_mean_queue_{nullptr};
  uint32_t *timestamp_reference_queue_{nullptr};
  T *mean2_queue_{nullptr};
  T *mean3_queue_{nullptr};
  T *mean4_queue_{nullptr};
};

}  // namespace statistics
}  // namespace esphome
