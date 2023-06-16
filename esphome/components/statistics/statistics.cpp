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
 *  - The mean is computed in a way to hopefully avoid catstrophic cancellation for large windows and/or large values
 *
 * Available computed over a sliding window:
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
 * Implemented by Kevin Ahrendt, June 2023
 */

#include "daba_lite.h"
#include "statistics.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

static const char *const TAG = "statistics";

void StatisticsComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Statistics:");

  LOG_SENSOR("  ", "Source Sensor", this->source_sensor_);

  ESP_LOGCONFIG(TAG, "  window_size: %u", this->window_size_);
  ESP_LOGCONFIG(TAG, "  send_every: %u", this->send_every_);
  ESP_LOGCONFIG(TAG, "  send_first_at: %u", this->send_at_);

  if (this->count_sensor_) {
    LOG_SENSOR("  ", "Count", this->count_sensor_);
  }

  if (this->max_sensor_) {
    LOG_SENSOR("  ", "Max", this->max_sensor_);
  }

  if (this->min_sensor_) {
    LOG_SENSOR("  ", "Min", this->min_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Mean", this->mean_sensor_);
  }

  if (this->variance_sensor_) {
    LOG_SENSOR("  ", "Variance", this->variance_sensor_);
  }

  if (this->std_dev_sensor_) {
    LOG_SENSOR("  ", "Standard Deviation", this->std_dev_sensor_);
  }

  if (this->covariance_sensor_) {
    LOG_SENSOR("  ", "Covariance", this->covariance_sensor_);
  }

  if (this->trend_sensor_) {
    LOG_SENSOR("  ", "Trend", this->trend_sensor_);
  }
}

void StatisticsComponent::setup() {
  // store aggregate data only needed for the configured sensors

  if (this->count_sensor_)
    this->partial_stats_queue_.enable_count();

  if (this->max_sensor_)
    this->partial_stats_queue_.enable_max();

  if (this->min_sensor_)
    this->partial_stats_queue_.enable_min();

  if (this->mean_sensor_) {
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
  }

  if ((this->variance_sensor_) || (this->std_dev_sensor_)) {
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_m2();
  }

  if (this->covariance_sensor_) {
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_timestamp_mean();
    this->partial_stats_queue_.enable_c2();
  }

  if (this->trend_sensor_) {
    this->partial_stats_queue_.enable_count();
    this->partial_stats_queue_.enable_mean();
    this->partial_stats_queue_.enable_timestamp_mean();
    this->partial_stats_queue_.enable_c2();
    this->partial_stats_queue_.enable_timestamp_m2();
  }

  // set the capacity of the DABA Lite queue for our window size
  //  - if not successful, then give an error and mark the component as failed
  if (!this->partial_stats_queue_.set_capacity(this->window_size_)) {
    ESP_LOGE(TAG, "Failed to allocate memory for sliding window aggregates of size %u", this->window_size_);
    this->mark_failed();
    return;
  }

  // when the source sensor updates, call handle_new_value_
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });

  // ensure we send our first reading when configured
  this->set_first_at(this->send_every_ - this->send_at_);
}

// given a new sensor measurements, evict if window is full, add new value to window, and update sensors
void StatisticsComponent::handle_new_value_(float value) {
  // if sliding window is larger than the capacity, evict until less
  while (this->partial_stats_queue_.size() >= this->window_size_) {
    this->partial_stats_queue_.evict();
  }

  // add new value to end of sliding window
  this->partial_stats_queue_.insert(value);

  // ensure we only push updates for the sensors based on the configuration
  if (++this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    if (this->count_sensor_)
      this->count_sensor_->publish_state(this->partial_stats_queue_.aggregated_count());

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

    if (this->mean_sensor_)
      this->mean_sensor_->publish_state(this->partial_stats_queue_.aggregated_mean());

    if (this->variance_sensor_)
      this->variance_sensor_->publish_state(this->partial_stats_queue_.aggregated_variance());

    if (this->std_dev_sensor_)
      this->std_dev_sensor_->publish_state(this->partial_stats_queue_.aggregated_std_dev());

    if (this->covariance_sensor_)
      this->covariance_sensor_->publish_state(this->partial_stats_queue_.aggregated_covariance());

    if (this->trend_sensor_)
      this->trend_sensor_->publish_state(this->partial_stats_queue_.aggregated_trend());
  }
}

}  // namespace statistics
}  // namespace esphome
