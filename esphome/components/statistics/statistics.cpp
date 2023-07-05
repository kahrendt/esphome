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

#include "aggregate_queue.h"
#include "daba_lite.h"
#include "running_singular.h"
#include "running_queue.h"

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

  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) {
    ESP_LOGCONFIG(TAG, "  statistics_type: sliding_window");
    ESP_LOGCONFIG(TAG, "  window_size: %u", this->window_size_);
  } else if (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_CONTINUOUS) {
    ESP_LOGCONFIG(TAG, "  statistics_type: chunked_continuous");
    ESP_LOGCONFIG(TAG, "  reset_every: %u", this->window_size_);
  }

  ESP_LOGCONFIG(TAG, "  send_every: %u", this->send_every_);
  ESP_LOGCONFIG(TAG, "  send_first_at: %u", this->send_at_);

  if (this->count_sensor_) {
    LOG_SENSOR("  ", "Count", this->count_sensor_);
  }

  if (this->covariance_sensor_) {
    LOG_SENSOR("  ", "Covariance", this->covariance_sensor_);
  }

  if (this->duration_sensor_) {
    LOG_SENSOR("  ", "Duration", this->duration_sensor_);
  }

  if (this->max_sensor_) {
    LOG_SENSOR("  ", "Max", this->max_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Mean", this->mean_sensor_);
  }

  if (this->min_sensor_) {
    LOG_SENSOR("  ", "Min", this->min_sensor_);
  }

  if (this->std_dev_sensor_) {
    LOG_SENSOR("  ", "Standard Deviation", this->std_dev_sensor_);
  }

  if (this->trend_sensor_) {
    LOG_SENSOR("  ", "Trend", this->trend_sensor_);
  }

  if (this->variance_sensor_) {
    LOG_SENSOR("  ", "Variance", this->variance_sensor_);
  }
}

void StatisticsComponent::setup() {
  // store aggregate data in the queues only necessary for the configured sensors
  EnabledAggregatesConfiguration config;

  if (this->count_sensor_)
    config.count = true;

  if (this->covariance_sensor_) {
    config.c2 = true;
    config.count = true;
    config.mean = true;
    config.timestamp_mean = true;
    config.timestamp_reference = true;
  }

  if (this->duration_sensor_)
    config.duration = true;

  if (this->max_sensor_) {
    config.max = true;
  }

  if (this->mean_sensor_) {
    config.count = true;
    config.mean = true;
  }

  if (this->min_sensor_) {
    config.min = true;
  }

  if ((this->std_dev_sensor_) || (this->variance_sensor_)) {
    config.count = true;
    config.m2 = true;
    config.mean = true;
  }

  if (this->trend_sensor_) {
    config.c2 = true;
    config.count = true;
    config.m2 = true;
    config.mean = true;
    config.timestamp_m2 = true;
    config.timestamp_mean = true;
    config.timestamp_reference = true;
  }

  // if averages are time weighted, then ensure we store duration info
  if (this->average_type_ == TIME_WEIGHTED_AVERAGE) {
    config.duration = true;
    config.duration_squared = true;
  }

  if ((this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) ||
      (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_SLIDING_WINDOW)) {
    this->queue_ = new DABALite();
  } else if (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_CONTINUOUS) {
    this->queue_ = new RunningQueue();
  } else if (this->statistics_type_ == STATISTICS_TYPE_CONTINUOUS) {
    this->queue_ = new RunningSingular();
  }

  this->queue_->set_capacity(this->window_size_, config);

  if (this->average_type_ == TIME_WEIGHTED_AVERAGE) {
    this->queue_->enable_time_weighted();
  }

  // On every source sensor update, call handle_new_value_()
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });

  // Ensure we send our first reading when configured
  this->set_first_at(this->send_every_ - this->send_at_);
}

void StatisticsComponent::insert_chunk_and_reset_(Aggregate value) {
  this->queue_->insert(value);

  // reset chunk
  this->current_chunk_aggregate_ = Aggregate();
  this->chunk_entries_ = 0;
  this->chunk_duration_ = 0;

  ++this->send_at_;  // only increment if a chunk has been inserted
}

void StatisticsComponent::reset() { this->queue_->clear(); }

// Given a new sensor measurement, evict if window is full, add new value to window, and update sensors
void StatisticsComponent::handle_new_value_(double value) {
  uint32_t now = millis();

  uint32_t duration = now - this->previous_timestamp_;
  float insert_value = value;

  if (this->average_type_ == TIME_WEIGHTED_AVERAGE) {
    insert_value = this->previous_value_;
  }

  this->previous_timestamp_ = now;
  this->previous_value_ = value;

  //////////////////////////////////////////////////
  // handle evicting elements if window is exceed //
  //////////////////////////////////////////////////
  //  If window_size_ == 0, then we have a running queue with no automatic reset, so we never evict/clear
  if (this->window_size_ > 0) {
    while (this->queue_->size() >= this->window_size_) {
      this->queue_->evict();
    }
  }

  ////////////////////////////
  // Add new value to queue //
  ////////////////////////////
  this->current_chunk_aggregate_ = this->current_chunk_aggregate_.combine_with(
      Aggregate(insert_value, duration), (this->average_type_ == TIME_WEIGHTED_AVERAGE));
  ++this->chunk_entries_;
  this->chunk_duration_ += duration;

  // If the chunk_size_ == 0, then our running chunk resets based on a duration
  if (this->chunk_size_ > 0) {
    if (this->chunk_entries_ >= this->chunk_size_) {
      this->insert_chunk_and_reset_(this->current_chunk_aggregate_);
    }
  } else {
    if (this->chunk_duration_ >= this->chunk_duration_size_) {
      this->insert_chunk_and_reset_(this->current_chunk_aggregate_);
    }
  }

  // Ensure we only push updates for the sensors based on the configuration
  // send_at_ counts the number of chunks inserted into the appropriate queue
  // after send_every_ chunks, each sensor is updated
  if (this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    Aggregate current_aggregate = this->queue_->compute_current_aggregate();

    if (this->count_sensor_)
      this->count_sensor_->publish_state(current_aggregate.get_count());

    if (this->covariance_sensor_) {
      double covariance_ms =
          current_aggregate.compute_covariance(this->average_type_ == TIME_WEIGHTED_AVERAGE, this->group_type_);
      double converted_covariance = covariance_ms / this->time_conversion_factor_;

      this->covariance_sensor_->publish_state(converted_covariance);
    }

    if (this->duration_sensor_)
      this->duration_sensor_->publish_state(current_aggregate.get_duration());

    if (this->max_sensor_) {
      float max = current_aggregate.get_max();
      if (std::isinf(max)) {  // default aggregated max for 0 measuremnts is -infinity, switch to NaN for HA
        this->max_sensor_->publish_state(NAN);
      } else {
        this->max_sensor_->publish_state(max);
      }
    }

    if (this->mean_sensor_)
      this->mean_sensor_->publish_state(current_aggregate.get_mean());

    if (this->min_sensor_) {
      float min = current_aggregate.get_min();
      if (std::isinf(min)) {  // default aggregated min for 0 measurements is infinity, switch to NaN for HA
        this->min_sensor_->publish_state(NAN);
      } else {
        this->min_sensor_->publish_state(min);
      }
    }

    if (this->std_dev_sensor_)
      this->std_dev_sensor_->publish_state(
          current_aggregate.compute_std_dev(this->average_type_ == TIME_WEIGHTED_AVERAGE, this->group_type_));

    if (this->trend_sensor_) {
      double trend_ms = current_aggregate.compute_trend();
      double converted_trend = trend_ms * this->time_conversion_factor_;

      this->trend_sensor_->publish_state(converted_trend);
    }

    if (this->variance_sensor_)
      this->variance_sensor_->publish_state(
          current_aggregate.compute_variance(this->average_type_ == TIME_WEIGHTED_AVERAGE, this->group_type_));

    if (this->mean2_sensor_)
      this->mean2_sensor_->publish_state(current_aggregate.get_mean2());
    if (this->mean3_sensor_)
      this->mean3_sensor_->publish_state(current_aggregate.get_mean3());
    if (this->mean4_sensor_)
      this->mean4_sensor_->publish_state(current_aggregate.get_mean4());
  }
}

}  // namespace statistics
}  // namespace esphome
