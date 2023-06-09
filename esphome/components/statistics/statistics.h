/*
 * ESPHome component to compute several summary statistics of a single sensor in an effecient (computationally and
 * memory-wise) manner
 *  - Works over a sliding window of incoming values; i.e., it is an online algorithm
 *  - Data is stored in a circular queue to be memory effecient by avoiding using std::deque
 *    - Currently uses std::vector (as the size of the sliding window is only passed as a variable with ESPHome, so it
 *      is dynamic in a sense)
 *      - The circular queue is implemented by keeping track of the indices (circular_queue_index.h)
 *    - Each summary statistic (or the value they are derived from) is stored in separate vectors
 *      - This avoids reserving large amounts of memory for nothing if some sensors are not configured
 *      - Configuring a sensor in ESPHome only stores the summary statistics it needs and no more
 *        - If multiple sensors require the same intermediate statistic, it is only stored once
 *  - Implements the DABA Lite algorithm on a circular_queue for computing online statistics
 *    - space requirements: n+2 (this implementation needs n+3; can be fixed... need to handle the index of the end of
 *      the queue being increased by 1)
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
 *      - potentially problematic for devies with long uptimes as it is based on the timestamp given by millis()
 *        - further experimentation is needed
 *        - I am researching a way to compute the covariance of two sets parallely only using a time delta from previous
 *          readings
 *  - trend: the slope of the line of best fit for the measurement values versus timestamps
 *      - can be be used as an approximate of the rate of change (derivative) of the measurements
 *      - potentially problematic for long uptimes as it uses covariance
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

  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }
  void set_sd_sensor(sensor::Sensor *sd_sensor) { this->sd_sensor_ = sd_sensor; }
  void set_variance_sensor(sensor::Sensor *variance_sensor) { this->variance_sensor_ = variance_sensor; }
  void set_count_sensor(sensor::Sensor *count_sensor) { this->count_sensor_ = count_sensor; }
  void set_trend_sensor(sensor::Sensor *trend_sensor) { this->trend_sensor_ = trend_sensor; }
  void set_covariance_sensor(sensor::Sensor *covariance_senesor) { this->covariance_sensor_ = covariance_senesor; }

  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }

 protected:
  void handle_new_value_(float value);

  sensor::Sensor *source_sensor_{nullptr};

  // requires struct members {count}
  sensor::Sensor *count_sensor_{nullptr};

  // requires struct member {max}
  sensor::Sensor *max_sensor_{nullptr};

  // require struct member {min}
  sensor::Sensor *min_sensor_{nullptr};

  // requires struct members {mean, count}
  sensor::Sensor *mean_sensor_{nullptr};

  // requires struct members {mean, count, m2}
  sensor::Sensor *sd_sensor_{nullptr};

  // requires struct members {mean, count, m2}
  sensor::Sensor *variance_sensor_{nullptr};

  // requires struct members {mean, count, t_mean, c_2}
  sensor::Sensor *covariance_sensor_{nullptr};

  // requires struct members {mean, count, t_mean, c_2, t_m2}
  sensor::Sensor *trend_sensor_{nullptr};

  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  DABALite partial_stats_queue_{};
};

}  // namespace statistics
}  // namespace esphome
