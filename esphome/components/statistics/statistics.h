/*
 * ESPHome component to compute several summary statistics of a single sensor in an effecient (computationally and
 * memory-wise) manner
 *  - Works over a sliding window of incoming values; i.e., it is an online algorithm
 *  - Data is stored in a circular queue to be memory effecient by avoiding using std::deque
 *    - The queue itself is an array allocated during componenet setup for the specified window size
 *      - The circular queue is implemented by keeping track of the indices (circular_queue_index.h)
 *   - Performs push_back and pop_front operations in constant time
 *    - Each summary statistic (or the aggregate they are derived from) is stored in a separate queue
 *      - This avoids reserving large amounts of pointless memory if some sensors are not configured
 *      - Configuring a sensor in ESPHome only stores the aggregates it needs and no more
 *        - If multiple sensors require the same intermediate aggregates, it is only stored once
 *  - Implements the DABA Lite algorithm over a circular queue for computing online statistics
 *    - space requirements: n+2 aggregates
 *    - time complexity: worse-case O(1)
 *    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
 *  - Uses variations of Welford's algorithm for parallel computing to find variance and covariance (with respect to
 *    time) to avoid catastrophic cancellation
 *  - The mean is computed to avoid catstrophic cancellation for large windows and/or large values
 *
 * Available statistics computed over the sliding window:
 *  - max: maximum measurement
 *  - min: minimum measurement
 *  - mean: average of the measurements
 *  - count: number of valid measurements in the window (component ignores NaN values in the window)
 *  - variance: sample variance of the measurements (Bessel's correction is applied)
 *  - std_dev: sample standard deviation of the measurements (Bessel's correction is applied)
 *  - covariance: sample covariance of the measurements compared to the timestamps of each reading
 *      - timestamps are stored as milliseconds
 *      - computed by keeping a rolling sum of the timestamps and a reference timestamp
 *      - the reference timestamp allows the rolling sum to always have one timestamp at 0
 *          - keeping offset rolling sum close to 0 should reduce the chance of floating point numbers losing
 *            signficant digits
 *      - integer operations are used as much as possible before switching to floating point arithemtic
 *  - trend: the slope of the line of best fit for the measurement values versus timestamps
 *      - can be be used as an approximation for the rate of change (derivative) of the measurements
 *      - computed using the covariance of timestamps versus measurements and the variance of timestamps
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, June 2023
 */

#pragma once

#include "aggregate.h"
#include "daba_lite.h"

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/sensor/sensor.h"

#include <vector>

namespace esphome {
namespace statistics {

enum StatisticsType {
  STATISTICS_TYPE_SLIDING_WINDOW,
  STATISTICS_TYPE_RUNNING,
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

  // source sensor of measurement data
  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  // sensors for aggregate statistics from sliding window
  void set_count_sensor(sensor::Sensor *count_sensor) { this->count_sensor_ = count_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }
  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_variance_sensor(sensor::Sensor *variance_sensor) { this->variance_sensor_ = variance_sensor; }
  void set_std_dev_sensor(sensor::Sensor *std_dev_sensor) { this->std_dev_sensor_ = std_dev_sensor; }
  void set_covariance_sensor(sensor::Sensor *covariance_senesor) { this->covariance_sensor_ = covariance_senesor; }
  void set_trend_sensor(sensor::Sensor *trend_sensor) { this->trend_sensor_ = trend_sensor; }

  // mimic ESPHome's current filter behavior
  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }

  void set_reset_every(size_t reset_every) { this->reset_every_ = reset_every; }

  void set_time_conversion_factor(TimeConversionFactor conversion_factor) {
    this->time_conversion_factor_ = conversion_factor;
  }

  void set_statistics_type(StatisticsType type) { this->statistics_type_ = type; }

 protected:
  // given a new sensor measurements, add it to window, evict if window is full, and update sensors
  void handle_new_value_(float value);

  TimeConversionFactor time_conversion_factor_;

  // source sensor of measurement data
  sensor::Sensor *source_sensor_{nullptr};

  // sensors for aggregate statistics from sliding window
  sensor::Sensor *count_sensor_{nullptr};
  sensor::Sensor *max_sensor_{nullptr};
  sensor::Sensor *min_sensor_{nullptr};
  sensor::Sensor *mean_sensor_{nullptr};
  sensor::Sensor *variance_sensor_{nullptr};
  sensor::Sensor *std_dev_sensor_{nullptr};
  sensor::Sensor *covariance_sensor_{nullptr};
  sensor::Sensor *trend_sensor_{nullptr};

  // DABA Lite implementation for storing measurements and computing aggregate statistics over the sliding window
  DABALite *partial_stats_queue_{nullptr};

  std::vector<Aggregate> running_queue_;

  // mimic ESPHome's current filters behavior
  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  size_t reset_every_{};
  size_t reset_count_{0};

  StatisticsType statistics_type_{STATISTICS_TYPE_SLIDING_WINDOW};

  void insert_running_queue(Aggregate new_aggregate);
  Aggregate compute_running_queue_aggregate();
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
