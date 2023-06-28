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

#include "aggregate.h"
#include "daba_lite.h"
#include "running_queue.h"
#include "statistics.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

#include <vector>

namespace esphome {
namespace statistics {

static const char *const TAG = "statistics";

void StatisticsComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Statistics:");

  LOG_SENSOR("  ", "Source Sensor", this->source_sensor_);

  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) {
    ESP_LOGCONFIG(TAG, "  statistics_type: sliding_window");
    ESP_LOGCONFIG(TAG, "  window_size: %u", this->window_size_);
  } else if (this->statistics_type_ == STATISTICS_TYPE_RUNNING) {
    ESP_LOGCONFIG(TAG, "  statistics_type: running");
    ESP_LOGCONFIG(TAG, "  reset_every: %u", this->reset_every_);
  }

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
  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) {
    // store aggregate data only necessary for the configured sensors
    DABAEnabledAggregateConfiguration config;

    if (this->count_sensor_)
      config.count = true;

    if (this->max_sensor_)
      config.max = true;

    if (this->min_sensor_)
      config.min = true;

    if (this->mean_sensor_) {
      config.count = true;
      config.mean = true;
    }

    if ((this->variance_sensor_) || (this->std_dev_sensor_)) {
      config.count = true;
      config.mean = true;
      config.m2 = true;
    }

    if (this->covariance_sensor_) {
      config.count = true;
      config.mean = true;
      config.timestamp_mean = true;
      config.c2 = true;
    }

    if (this->trend_sensor_) {
      config.count = true;
      config.mean = true;
      config.timestamp_mean = true;
      config.c2 = true;
      config.timestamp_m2 = true;
    }
    this->partial_stats_queue_.set_enabled_aggregates(config);

    if (!this->partial_stats_queue_.set_capacity(this->window_size_)) {
      ESP_LOGE(TAG, "Failed to allocate memory for sliding window aggregates of size %u", this->window_size_);
      this->mark_failed();
      return;
    }

  } else {
    if (!this->running_queue_.set_capacity(this->reset_every_)) {
      ESP_LOGE(TAG, "Failed to allocate memory for running aggregates.");
      this->mark_failed();
      return;
    }
  }

  // On every source sensor update, call handle_new_value_()
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });

  // Ensure we send our first reading when configured
  this->set_first_at(this->send_every_ - this->send_at_);
}

void StatisticsComponent::reset() {
  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW)
    this->partial_stats_queue_.clear();
  else if (this->statistics_type_ == STATISTICS_TYPE_RUNNING)
    this->running_queue_.clear();
}

// Given a new sensor measurement, evict if window is full, add new value to window, and update sensors
void StatisticsComponent::handle_new_value_(float value) {
  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) {
    // If sliding window is larger than the capacity, evict until less
    while (this->partial_stats_queue_.size() >= this->window_size_) {
      this->partial_stats_queue_.evict();
    }

    // Add new value to end of sliding window
    this->partial_stats_queue_.insert(value);
  } else {
    this->running_queue_.insert(value);
    ++this->reset_count_;
  }

  // Ensure we only push updates for the sensors based on the configuration
  if (++this->send_at_ >= this->send_every_) {
    Aggregate current_aggregate;

    this->send_at_ = 0;

    if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW)
      current_aggregate = this->partial_stats_queue_.get_current_aggregate();
    else if (this->statistics_type_ == STATISTICS_TYPE_RUNNING)
      current_aggregate = this->running_queue_.compute_current_aggregate();

    if (this->count_sensor_)
      this->count_sensor_->publish_state(current_aggregate.get_count());

    if (this->max_sensor_) {
      float max = current_aggregate.get_max();
      if (std::isinf(max)) {  // default aggregated max for 0 measuremnts is -infinity, switch to NaN for HA
        this->max_sensor_->publish_state(NAN);
      } else {
        this->max_sensor_->publish_state(max);
      }
    }

    if (this->min_sensor_) {
      float min = current_aggregate.get_min();
      if (std::isinf(min)) {  // default aggregated min for 0 measurements is infinity, switch to NaN for HA
        this->min_sensor_->publish_state(NAN);
      } else {
        this->min_sensor_->publish_state(min);
      }
    }

    if (this->mean_sensor_)
      this->mean_sensor_->publish_state(current_aggregate.get_mean());

    if (this->variance_sensor_)
      this->variance_sensor_->publish_state(current_aggregate.compute_variance());

    if (this->std_dev_sensor_)
      this->std_dev_sensor_->publish_state(current_aggregate.compute_std_dev());

    if (this->covariance_sensor_) {
      float covariance_ms = current_aggregate.compute_covariance();
      float converted_covariance = covariance_ms / this->time_conversion_factor_;

      this->covariance_sensor_->publish_state(converted_covariance);
    }

    if (this->trend_sensor_) {
      float trend_ms = current_aggregate.compute_trend();
      float converted_trend = trend_ms * this->time_conversion_factor_;

      this->trend_sensor_->publish_state(converted_trend);
    }
  }

  if (this->reset_count_ == this->reset_every_) {
    this->reset();
    this->reset_count_ = 0;
  }
}

}  // namespace statistics
}  // namespace esphome
