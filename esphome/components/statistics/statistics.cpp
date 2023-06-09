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

#include "statistics.h"
#include "daba_lite.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

static const char *const TAG = "statistics";

void StatisticsComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Statistics:");

  ESP_LOGCONFIG(TAG, "  window_size: %u", this->window_size_);
  ESP_LOGCONFIG(TAG, "  send_every: %u", this->send_every_);
  ESP_LOGCONFIG(TAG, "  send_first_at: %u", this->send_at_);

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Mean", this->mean_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Min", this->min_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Max", this->max_sensor_);
  }

  if (this->sd_sensor_) {
    LOG_SENSOR("  ", "Standard Deviation", this->sd_sensor_);
  }

  if (this->variance_sensor_) {
    LOG_SENSOR("  ", "Variance", this->variance_sensor_);
  }

  if (this->count_sensor_) {
    LOG_SENSOR("  ", "Count", this->count_sensor_);
  }

  if (this->trend_sensor_) {
    LOG_SENSOR("  ", "Trend", this->trend_sensor_);
  }
}

void StatisticsComponent::setup() {
  if (this->max_sensor_)
    this->partial_stats_queue_.enable_max();
  if (this->min_sensor_)
    this->partial_stats_queue_.enable_min();
  if (this->count_sensor_)
    this->partial_stats_queue_.enable_count();
  if (this->mean_sensor_) {
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_count();
  }
  if ((this->variance_sensor_) || (this->sd_sensor_)) {
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_m2();
  }
  if (this->covariance_sensor_) {
    this->partial_stats_queue_.enable_c2();
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_t_mean();
  }
  if (this->trend_sensor_) {
    this->partial_stats_queue_.enable_c2();
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_t_mean();
    this->partial_stats_queue_.enable_t_m2();
  }

  this->partial_stats_queue_.set_capacity(this->window_size_);

  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}

void StatisticsComponent::handle_new_value_(float value) {
  while (this->partial_stats_queue_.size() >= this->window_size_) {
    this->partial_stats_queue_.evict();
  }

  this->partial_stats_queue_.insert(value);

  if (++this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    if (this->mean_sensor_)
      this->mean_sensor_->publish_state(this->partial_stats_queue_.aggregated_mean());

    if (this->max_sensor_) {
      float max = this->partial_stats_queue_.aggregated_max();
      if (std::isinf(max)) {  // default aggregated max for 0 readings is -infinity, switch to NaN for HA
        this->max_sensor_->publish_state(NAN);
      } else {
        this->max_sensor_->publish_state(max);
      }
    }

    if (this->min_sensor_) {
      float min = this->partial_stats_queue_.aggregated_min();
      if (std::isinf(min)) {  // default aggregated min for 0 readings is infinity, switch to NaN for HA
        this->min_sensor_->publish_state(NAN);
      } else {
        this->min_sensor_->publish_state(min);
      }
    }

    if (this->sd_sensor_)
      this->sd_sensor_->publish_state(this->partial_stats_queue_.aggregated_std_dev());

    if (this->variance_sensor_)
      this->variance_sensor_->publish_state(this->partial_stats_queue_.aggregated_variance());

    if (this->count_sensor_)
      this->count_sensor_->publish_state(this->partial_stats_queue_.aggregated_count());

    if (this->trend_sensor_)
      this->trend_sensor_->publish_state(this->partial_stats_queue_.aggregated_trend());

    if (this->covariance_sensor_)
      this->covariance_sensor_->publish_state(this->partial_stats_queue_.aggregated_covariance());
  }
}

}  // namespace statistics
}  // namespace esphome
