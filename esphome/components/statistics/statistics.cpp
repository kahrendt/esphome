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
  ESP_LOGCONFIG(TAG, "Statistics Component:");

  LOG_SENSOR("  ", "Source Sensor:", this->source_sensor_);

  if (this->statistics_type_ == STATISTICS_TYPE_SLIDING_WINDOW) {
    ESP_LOGCONFIG(TAG, "  Statistics Type: sliding_window");
    ESP_LOGCONFIG(TAG, "  Window Size: %u", this->window_size_);
  } else if (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_SLIDING_WINDOW) {
    ESP_LOGCONFIG(TAG, "  Statistics Type: chunked_sliding_window");
    ESP_LOGCONFIG(TAG, "  Chunks in Window: %u", this->window_size_);
    if (this->chunk_size_) {
      ESP_LOGCONFIG(TAG, "  Measurements per Chunk: %u", this->chunk_size_);
    } else {
      ESP_LOGCONFIG(TAG, "  Duration of Chunk: %u ms", this->chunk_duration_size_);
    }
  } else if (this->statistics_type_ == STATISTICS_TYPE_CONTINUOUS) {
    ESP_LOGCONFIG(TAG, "  Statistics Type: continuous");
    ESP_LOGCONFIG(TAG, "  Chunks Before Reset: %u", this->window_size_);
    if (this->chunk_size_) {
      ESP_LOGCONFIG(TAG, "  Measurements per Chunk: %u", this->chunk_size_);
    } else {
      ESP_LOGCONFIG(TAG, "  Duration of Chunk: %u ms", this->chunk_duration_size_);
    }
  } else if (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_CONTINUOUS) {
    ESP_LOGCONFIG(TAG, "  Statistics Type: chunked_continuous");
    ESP_LOGCONFIG(TAG, "  Measurements Before Reset: %u", this->window_size_);
  }

  ESP_LOGCONFIG(TAG, "  Send Every: %u", this->send_every_);

  // To-Do: IMPLEMENT!
  ESP_LOGCONFIG(TAG, "  Average Type: ");
  ESP_LOGCONFIG(TAG, "  Time Conversion Factor: ");

  if (this->count_sensor_) {
    LOG_SENSOR("  ", "Count Sensor:", this->count_sensor_);
  }

  if (this->covariance_sensor_) {
    LOG_SENSOR("  ", "Covariance Sensor:", this->covariance_sensor_);
  }

  if (this->duration_sensor_) {
    LOG_SENSOR("  ", "Duration Sensor:", this->duration_sensor_);
  }

  if (this->max_sensor_) {
    LOG_SENSOR("  ", "Max Sensor:", this->max_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Mean Sensor:", this->mean_sensor_);
  }

  if (this->min_sensor_) {
    LOG_SENSOR("  ", "Min Sensor:", this->min_sensor_);
  }

  if (this->std_dev_sensor_) {
    LOG_SENSOR("  ", "Standard Deviation Sensor:", this->std_dev_sensor_);
  }

  if (this->trend_sensor_) {
    LOG_SENSOR("  ", "Trend Sensor:", this->trend_sensor_);
  }

  if (this->variance_sensor_) {
    LOG_SENSOR("  ", "Variance Sensor:", this->variance_sensor_);
  }
}

void StatisticsComponent::setup() {
  // store aggregate data in the queues only necessary for the configured sensors
  EnabledAggregatesConfiguration config;

  if (this->covariance_sensor_) {
    config.c2 = true;
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
    config.mean = true;
  }

  if (this->min_sensor_) {
    config.min = true;
  }

  if ((this->std_dev_sensor_) || (this->variance_sensor_)) {
    config.m2 = true;
    config.mean = true;
  }

  if (this->trend_sensor_) {
    config.c2 = true;
    config.m2 = true;
    config.mean = true;
    config.timestamp_m2 = true;
    config.timestamp_mean = true;
    config.timestamp_reference = true;
  }

  // if averages are time weighted, then ensure we store duration info
  if (this->is_time_weighted_()) {
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

  if (!this->queue_->set_capacity(this->window_size_, config)) {
    ESP_LOGE(TAG, "Failed to allocate memory for statistics.");
    this->mark_failed();
  }

  if (this->is_time_weighted_()) {
    this->queue_->enable_time_weighted();
  }

  // On every source sensor update, call handle_new_value_()
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });

  // Ensure we send our first reading when configured
  this->set_first_at(this->send_every_ - this->send_at_);
}

void StatisticsComponent::reset() { this->queue_->clear(); }

// Given a new sensor measurement, evict if window is full, add new value to window, and update sensors
void StatisticsComponent::handle_new_value_(double value) {
  uint32_t now = millis();

  uint32_t duration = now - this->previous_timestamp_;
  float insert_value = value;

  if (this->is_time_weighted_()) {
    insert_value = this->previous_value_;
  }

  this->previous_timestamp_ = now;
  this->previous_value_ = value;

  ////////////////////////////////////////////////
  // Evict elements or reset queue if too large //
  ////////////////////////////////////////////////
  //  If window_size_ == 0, then we have a running type queue with no automatic reset, so we never evict/clear
  if (this->window_size_ > 0) {
    while (this->queue_->size() >= this->window_size_) {
      this->queue_->evict();
    }
  }

  ////////////////////////////
  // Add new chunk to a queue //
  ////////////////////////////
  this->running_chunk_aggregate_ =
      this->running_chunk_aggregate_.combine_with(Aggregate(insert_value, duration, now), this->is_time_weighted_());
  ++this->running_chunk_count_;
  this->running_chunk_duration += duration;

  if (this->is_running_chunk_ready_()) {
    this->queue_->insert(this->running_chunk_aggregate_);

    // Reset counters and chunk to a null measurement
    this->running_chunk_aggregate_ = Aggregate();
    this->running_chunk_count_ = 0;
    this->running_chunk_duration = 0;

    ++this->send_at_;  // only incremented if a chunk has been inserted
  }

  // Ensure we only push updates at the rate configured
  //  - send_at_ counts the number of chunks inserted into the appropriate queue
  //  - after send_every_ chunks, each sensor is updated
  if (this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    Aggregate current_aggregate = this->queue_->compute_current_aggregate();

    if (this->count_sensor_)
      this->count_sensor_->publish_state(current_aggregate.get_count());

    if (this->covariance_sensor_) {
      double covariance_ms = current_aggregate.compute_covariance(this->is_time_weighted_(), this->group_type_);
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
          current_aggregate.compute_std_dev(this->is_time_weighted_(), this->group_type_));

    if (this->trend_sensor_) {
      double trend_ms = current_aggregate.compute_trend();
      double converted_trend = trend_ms * this->time_conversion_factor_;

      this->trend_sensor_->publish_state(converted_trend);
    }

    if (this->variance_sensor_)
      this->variance_sensor_->publish_state(
          current_aggregate.compute_variance(this->is_time_weighted_(), this->group_type_));
  }
}

// Returns true if the summary statistics should be weighted based on the measurement's duration
inline bool StatisticsComponent::is_time_weighted_() { return (this->average_type_ == TIME_WEIGHTED_AVERAGE); }

// Determines whether the current running aggregate chunk is full; i.e., has aggregated enough measurements or the
// configured duration has been exceeded
inline bool StatisticsComponent::is_running_chunk_ready_() {
  // If the chunk_size_ == 0, then our running chunk resets based on a duration
  if (this->chunk_size_ > 0) {
    if (this->running_chunk_count_ >= this->chunk_size_) {
      return true;
    }
  } else {
    if (this->running_chunk_duration >= this->chunk_duration_size_) {
      if (this->running_chunk_count_ > 0)  // ensure we have aggregated at least one measurement in this timespan
        return true;
    }
  }

  return false;
}

}  // namespace statistics
}  // namespace esphome
