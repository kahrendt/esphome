/*
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July 2023
 */

#pragma once

#include "aggregate.h"

namespace esphome {
namespace statistics {

/// @brief Configures which statistics will be stored in the queue
struct EnabledAggregatesConfiguration {
  bool c2{false};
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
  //////////////////////////////////////
  // Virtual methods to be overloaded //
  //////////////////////////////////////

  /// @brief Set the queue's capacity and preallocates memory.
  virtual bool set_capacity(size_t capacity, EnabledAggregatesConfiguration config) = 0;

  /// @brief Clear all aggregates in the queue.
  virtual void clear() = 0;

  /// @brief Remove the oldest Aggregate in the queue (if sliding window) or clear it (if continuous).
  virtual void evict() = 0;

  /// @brief Insert aggregate at end of queue.
  virtual void insert(Aggregate value) = 0;

  /// @brief Return the aggregate summarizing all entries in the queue.
  virtual Aggregate compute_current_aggregate() = 0;

  /////////////////////////////
  // Directly usable methods //
  /////////////////////////////

  /** Store an aggregate at an index in the queue.
   *
   * Only the configured statistics are stored, the rest are ignored.
   * @param value the aggregate to be emplaced
   * @param index the index location in the queue to emplace <value>
   */
  void emplace(const Aggregate &value, size_t index);

  /** Retrieve the aggregate from a particular index in the queue.
   *
   * Only the configured statistics are retreived, the rest are default values.
   * @param index the index location in the queue to retrieve an aggregate
   * @return the aggregate from <index>
   */
  Aggregate lower(size_t index);

  /** Allocate memory for queue.
   *
   * Only allocates memory for statistics set in <config>. Attempts to allocate in ExternalRAM if present, otherwise it
   * falls back to built-in memory.
   * @param capacity the number of aggregates that can be stored in the queue
   * @param config which summary statistics should be saved into the queue
   * @return true if memory was sucecessfuly allocated, false otherwise
   */
  bool allocate_memory(size_t capacity, EnabledAggregatesConfiguration config);

  /** Return the number of aggregates inserted into queue.
   *
   * @return amount of aggregates inserted and aggregated into queue
   */
  size_t size() const { return this->size_; };

  /// Enable averages to be weighted by the measurement's duration.
  void enable_time_weighted() { this->time_weighted_ = true; }

 protected:
  // Determines whether measurements hould be weighted by duration (true) or be of equal weight (false)
  bool time_weighted_{false};

  // Stores the number of aggregates inserted into the queue
  size_t size_{0};

  // Queues for storing aggregate statistics
  size_t *count_queue_{nullptr};
  size_t *duration_queue_{nullptr};
  size_t *duration_squared_queue_{nullptr};

  uint32_t *timestamp_reference_queue_{nullptr};

  float *max_queue_{nullptr};
  float *mean_queue_{nullptr};
  float *min_queue_{nullptr};

  // By experimentation, using doubles for these improves accuracy in a measurable way
  double *c2_queue_{nullptr};
  double *m2_queue_{nullptr};
  double *timestamp_m2_queue_{nullptr};
  double *timestamp_mean_queue_{nullptr};
};

}  // namespace statistics
}  // namespace esphome
