/*
 * ESPHome component to compute several summary statistics of a single sensor in an effecient (computationally and
 * memory-wise) manner
 *  - Works over a sliding window of incoming values; i.e., it is an online algorithm
 *  - Data is stored in a circular queue to be memory effecient by avoiding using std::deque
 *    - The queue itself is an array allocated during componenet setup for the specified window size
 *      - The circular queue is implemented by keeping track of the indices (circular_queue_index.h)
 *   - Performs push_back and pop_front operations in constant time*
 *    - Each summary statistic (or the value they are derived from) is stored in a separate queue
 *      - This avoids reserving large amounts of useless memory if some sensors are not configured
 *      - Configuring a sensor in ESPHome only stores the summary statistics it needs and no more
 *        - If multiple sensors require the same intermediate statistic, it is only stored once
 *  - Implements the DABA Lite algorithm on a circular_queue for computing online statistics
 *    - space requirements: n+2
 *    - time complexity: worse-case O(1)
 *    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
 *  - Uses variations of Welford's algorithm for parallel computing to find variance and covariance (with respect to
 *    time) to avoid catastrophic cancellation
 *  - mean is computed in a way to hopefully avoid catstrophic cancellation for large windows and large values
 *
 * Available computed over a sliding window:
 *  - max: maximum measurement
 *  - min: minimum measurement
 *  - mean: average of the measurements
 *  - count: number of valid measurements in the window (component ignores NaN values)
 *  - variance: sample variance of the measurements
 *  - sd: sample standard deviation of the measurements
 *  - covariance: sample covariance of the measurements compared to the timestamps of each reading
 *      - timestamps are stored as milliseconds
 *      - computed by keeping a rolling sum of the timestamps and a reference timestamp
 *      - the reference timestamp allows the rolling sum to always have one timestamp at 0
 *          - keeping offset rolling sum close to 0 should reduce the chance of floating point operations losing
 *            signficant digits
 *      - integer operations are used as much as possible before switching to floating point arithemtic
 *      - ?potential issues when millis() rolls over?
 *  - trend: the slope of the line of best fit for the measurement values versus timestamps
 *      - can be be used as an approximate of the rate of change (derivative) of the measurements
 *      - computed using the covariance of timestamps versus measurements and the variance of timestamps
 *      - ?potential issues when millis() rolls over?
 *
 * To-Do: Verify that millis() rollover is handled
 *
 * Implemented by Kevin Ahrendt, June 2023
 */

#pragma once

#include "daba_lite.h"

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

class StatisticsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;

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

 protected:
  // given a new sensor measurements, add it to window, evict if window is full, and update sensors
  void handle_new_value_(float value);

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

  // mimic ESPHome's current filters behavior
  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  // DABA Lite implementation for storing measurements and computing aggregate statistics over the sliding window
  DABALite partial_stats_queue_{};
};

}  // namespace statistics
}  // namespace esphome
