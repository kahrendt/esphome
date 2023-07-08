/*
 * ESPHome component to compute several summary statistics for a set of measurements from a sensor in an effecient
 * (computationally and memory-wise) manner while being numerically stable and accurate. The set of measurements can be
 * collected over a sliding window or as a resetable running total. Each measurement can be set to have equal weight or
 * to be weighted by their duration.
 *
 * Available statistics as sensors
 *  - count: number of valid measurements in the window (component ignores NaN values in the window)
 *  - covariance: sample or population covariance of the measurements compared to the timestamps of each reading
 *  - duration: the duration in milliseconds between the first and last measurement's timestamps
 *  - min: minimum of the set of measurements
 *  - mean: average of the set of measurements
 *  - max: maximum of the set of measurements*
 *  - std_dev: sample or population standard deviation of the set of measurements
 *  - trend: the slope of the line of best fit for the measurement values versus timestamps
 *      - can be be used as an approximation for the rate of change (derivative) of the measurements
 *      - computed using the covariance of timestamps versus measurements and the variance of timestamps
 *  - variance: sample or population variance of the set of measurements
 *
 * Term and definitions used in this component:
 *  - measurement: a single reading from a sensor
 *  - observation: a single reading from a sensor
 *  - set of measurements: a collection of measurements from a sensor
 *    - it can be the null set; i.e., it does not contain a measurement
 *  - summary statistic: a numerical value that summarizes a set of measurements
 *  - aggregate: a collection of summary statistics for a set of measurements
 *  - to aggregate: adding a measurement to the set of measurements and updating the aggregate to include the new
 *    measurement
 *  - queue: a set of aggregates that can compute all summmary statistics for all aggregates in the set combined
 *  - to insert: adding an aggregate to a queue
 *  - to evict: remove the oldest aggregate from a queue
 *  - chunk: an aggregate that specifically aggregates incoming measurements and inserted into a queue
 *  - chunk size: the number of measurements to aggregate in a chunk before it is inserted into a queue
 *  - chunk duration: the timespan between the first and last measurement in a chunk before being inserted into a queue
 *  - sliding window queue: a queue that can insert new aggregates and evict the oldest aggregate
 *  - sliding window aggregate: an agggregate that stores the summary statistics for all aggregates in a sliding
 *    window queue
 *  - continuous queue: a queue that can only insert new aggregates and be cleared
 *  - continuous aggregate: an aggregate that stores the summary statistics for all aggregates in a continuous queue
 *  - simple average: every measurement has equal weight when computing summary statistics
 *  - time-weighted average: each measurement is weighted by the time until the next measurement is observed
 *
 * Component code structure: (see specific header files for more detailed descriptions):
 *  - statistics.h - Statistics is a class that sets up the component by allocating memory for a configured queue and
 *    handles new measurements
 *  - aggregate.h - Aggregate is a class that stores a collection of summary statistics and can combine two aggregates
 *    into one
 *  - aggregate_queue.h - AggregateQueue is a class that allocates memory for a set of aggregates for the enabled
 *    sensors, as well as stores and retrieves aggregates from the memory
 *  - daba_lite_queue.h - DABALiteQueue is a child of AggregateQueue. It implements the De-Amortized Banker's Aggregator
 *    (DABA) Lite algorithm for sliding window queues
 *  - continuous_queue.h - ContinuousQueue is a child of AggregateQueue. It stores aggregates and combines them when
 *    they have the same number of measurements. Numerically stable for long-term aggregation of measurements in a
 *    continuous queue, but not as effecient computationally or memory-wise
 *  - continous_singular.h - ContinuousSingular is a child of AggregateQueue. It stores a single running aggregate.
 *    Memory and computationally effecient for continuous aggregates, but is not numerically stable for long-term
 *    aggregation of measurements.
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June and July of 2023
 */

#pragma once

#include "aggregate.h"
#include "aggregate_queue.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

// from esphome/components/servo/servo.h
// see https://github.com/esphome/esphome/pull/3416
extern uint32_t global_statistics_id;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

enum AverageType {
  SIMPLE_AVERAGE,
  TIME_WEIGHTED_AVERAGE,
};

enum StatisticsType {
  STATISTICS_TYPE_SLIDING_WINDOW,
  STATISTICS_TYPE_CHUNKED_SLIDING_WINDOW,
  STATISTICS_TYPE_CONTINUOUS,
  STATISTICS_TYPE_CHUNKED_CONTINUOUS,
};

enum TimeConversionFactor {
  FACTOR_MS = 1,          // timestamps already are in ms
  FACTOR_S = 1000,        // 1000 ms per second
  FACTOR_MIN = 60000,     // 60000 ms per minute
  FACTOR_HOUR = 3600000,  // 3600000 ms per hour
  FACTOR_DAY = 86400000,  // 86400000 ms per day
};

class StatisticsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;

  void reset();

  // source sensor of measurements
  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  // sensors for aggregate statistics
  void set_count_sensor(sensor::Sensor *count_sensor) { this->count_sensor_ = count_sensor; }
  void set_covariance_sensor(sensor::Sensor *covariance_senesor) { this->covariance_sensor_ = covariance_senesor; }
  void set_duration_sensor(sensor::Sensor *duration_sensor) { this->duration_sensor_ = duration_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }
  void set_std_dev_sensor(sensor::Sensor *std_dev_sensor) { this->std_dev_sensor_ = std_dev_sensor; }
  void set_trend_sensor(sensor::Sensor *trend_sensor) { this->trend_sensor_ = trend_sensor; }
  void set_variance_sensor(sensor::Sensor *variance_sensor) { this->variance_sensor_ = variance_sensor; }

  // mimic ESPHome's current filter behavior
  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }

  void set_chunk_size(size_t size) { this->chunk_size_ = size; }
  void set_chunk_duration(uint32_t time_delta) { this->chunk_duration_ = time_delta; }

  void set_continuous_reset_duration(size_t duration) { this->continuous_queue_reset_duration_ = duration; }

  void set_average_type(AverageType type) { this->average_type_ = type; }
  void set_group_type(GroupType type) { this->group_type_ = type; }
  void set_statistics_type(StatisticsType type) { this->statistics_type_ = type; }
  void set_time_conversion_factor(TimeConversionFactor conversion_factor) {
    this->time_conversion_factor_ = conversion_factor;
  }

  void set_restore(bool restore) { this->restore_ = restore; }

 protected:
  // source sensor of measurement data
  sensor::Sensor *source_sensor_{nullptr};

  // sensors for aggregate statistics from sliding window
  sensor::Sensor *count_sensor_{nullptr};
  sensor::Sensor *covariance_sensor_{nullptr};
  sensor::Sensor *duration_sensor_{nullptr};
  sensor::Sensor *max_sensor_{nullptr};
  sensor::Sensor *mean_sensor_{nullptr};
  sensor::Sensor *min_sensor_{nullptr};
  sensor::Sensor *std_dev_sensor_{nullptr};
  sensor::Sensor *trend_sensor_{nullptr};
  sensor::Sensor *variance_sensor_{nullptr};

  AggregateQueue *queue_{nullptr};

  Aggregate running_chunk_aggregate_{};

  // mimic ESPHome's current filters behavior
  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  size_t chunk_size_{1};       // amount of measurements stored in a chunk before being inserted into the queue
  uint32_t chunk_duration_{};  // duration of measurements stored in a chunk before being inserted into the queue

  size_t running_chunk_count_{0};       // amount of measurements currently stored in the running aggregate chunk
  uint32_t running_chunk_duration_{0};  // duration of measurements currently stored in the running aggregate chunk

  size_t continuous_queue_duration_{0};
  size_t continuous_queue_reset_duration_{0};

  AverageType average_type_{};  // either simple or time-weighted
  GroupType group_type_{};      // measurements come from either a population or sample
  StatisticsType statistics_type_{};
  TimeConversionFactor time_conversion_factor_{};  // covariance and trend have a unit involving a time unit

  // if the aggregates are time-weighted, these store info about the previous observation
  float previous_value_{NAN};
  uint32_t previous_timestamp_{0};

  bool restore_{false};
  ESPPreferenceObject pref_;

  // given a new sensor measurements, add it to queue, evict/clear if queue is full, and update sensors
  void handle_new_value_(double value);

  inline bool is_time_weighted_();

  inline bool is_running_chunk_ready_();
};

// Based on the integration component reset action
template<typename... Ts> class ResetAction : public Action<Ts...> {
 public:
  explicit ResetAction(StatisticsComponent *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->reset(); }

 protected:
  StatisticsComponent *parent_;
};

}  // namespace statistics
}  // namespace esphome
