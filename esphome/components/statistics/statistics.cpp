/*
To-do:
  - test whether time weighted averages with no sensor updates in a chunk duration is handled properly
  - verify accuracy of stats using mqtt and python
  - add/improve comments in aggregate_queue.h
  - update documentation draft in esphome-docs repository
    - add table describing when each type of queue should be used
  - use a consistent commenting style
  - spell/grammar check comments and documentation
  - write a cookbook documentation example for humidity detection using a trend sensor
*/

#include "aggregate.h"

#include "aggregate_queue.h"
#include "daba_lite_queue.h"
#include "continuous_singular.h"
#include "continuous_queue.h"

#include "statistics.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

static const char *const TAG = "statistics";
uint32_t global_statistics_id = 3141044017ULL;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

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
      ESP_LOGCONFIG(TAG, "  Duration of Chunk: %u ms", this->chunk_duration_);
    }
  } else if (this->statistics_type_ == STATISTICS_TYPE_CONTINUOUS) {
    ESP_LOGCONFIG(TAG, "  Statistics Type: continuous");
    ESP_LOGCONFIG(TAG, "  Chunks Before Reset: %u", this->window_size_);
    if (this->chunk_size_) {
      ESP_LOGCONFIG(TAG, "  Measurements per Chunk: %u", this->chunk_size_);
    } else {
      ESP_LOGCONFIG(TAG, "  Duration of Chunk: %u ms", this->chunk_duration_);
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
    this->queue_ = new DABALiteQueue();
  } else if (this->statistics_type_ == STATISTICS_TYPE_CHUNKED_CONTINUOUS) {
    this->queue_ = new ContinuousQueue();
  } else if (this->statistics_type_ == STATISTICS_TYPE_CONTINUOUS) {
    this->queue_ = new ContinuousSingular();
  }

  if (!this->queue_->set_capacity(this->window_size_, config)) {
    ESP_LOGE(TAG, "Failed to allocate memory for statistical aggregates.");
    this->mark_failed();
  }

  if (this->is_time_weighted_()) {
    this->queue_->enable_time_weighted();
  }

  if (this->restore_) {
    this->pref_ = global_preferences->make_preference<Aggregate>(global_statistics_id);
    global_statistics_id++;

    Aggregate restored_value;
    this->pref_.load(&restored_value);

    this->queue_->insert(restored_value);
  }

  // On every source sensor update, call handle_new_value_()
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });

  // Ensure we send our first reading when configured
  this->set_first_at(this->send_every_ - this->send_at_);
}

void StatisticsComponent::reset() {
  this->queue_->clear();                         // clear the queue
  this->running_chunk_aggregate_ = Aggregate();  // reset the running aggregate to the identity
}

void StatisticsComponent::handle_new_value_(float value) {
  //////////////////////////////////////////////
  // Prepare incoming values to be aggregated //
  //////////////////////////////////////////////

  uint32_t now = millis();
  uint32_t duration_since_last_measurement = now - this->previous_timestamp_;

  float insert_value = value;

  // If averages are time weighted, then insert the previous value as it has duration of duration_since_last_measurement
  if (this->is_time_weighted_()) {
    insert_value = this->previous_value_;
  }

  this->previous_timestamp_ = now;
  this->previous_value_ = value;

  ////////////////////////////////////////////////
  // Evict elements or reset queue if too large //
  ////////////////////////////////////////////////

  //  If window_size_ == 0, then we have a continuous type queue with no automatic reset, so we never evict/clear
  if (this->window_size_ > 0) {
    while (this->queue_->size() >= this->window_size_) {
      this->queue_->evict();  // evict is equivalent to clearing the queue for continuous_queue and continuous_singular
    }
  }

  // If the continuous_queue_reset_duration_ == 0, then we are not a continuous queue or we are not resetting by
  // duration
  if (this->continuous_queue_reset_duration_ > 0) {
    if (this->continuous_queue_duration_ >= this->continuous_queue_reset_duration_) {
      this->queue_->clear();
      this->continuous_queue_duration_ = 0;
    }
  }

  ////////////////////////////////////////////
  // Aggregate new value into running chunk //
  ////////////////////////////////////////////

  this->running_chunk_aggregate_ = this->running_chunk_aggregate_.combine_with(
      Aggregate(insert_value, duration_since_last_measurement, now), this->is_time_weighted_());

  ++this->running_chunk_count_;
  this->running_chunk_duration_ += duration_since_last_measurement;
  this->continuous_queue_duration_ += duration_since_last_measurement;

  //////////////////////////////
  // Add new chunk to a queue //
  //////////////////////////////

  if (this->is_running_chunk_ready_()) {
    this->queue_->insert(this->running_chunk_aggregate_);

    // Reset counters and chunk to a null measurement
    this->running_chunk_aggregate_ = Aggregate();
    this->running_chunk_count_ = 0;
    this->running_chunk_duration_ = 0;

    ++this->send_at_;  // only incremented if a chunk has been inserted
  }

  ////////////////////////////////////
  // Publish and save sensor values //
  ////////////////////////////////////

  if (this->send_at_ >= this->send_every_) {
    // Ensures we only push updates at the rate configured
    //  - send_at_ counts the number of chunks inserted into the appropriate queue
    //  - after send_every_ chunks, each sensor is updated

    this->send_at_ = 0;  // reset send_at_

    this->publish_and_save_(this->queue_->compute_current_aggregate());
  }
}

void StatisticsComponent::publish_and_save_(Aggregate value) {
  // Publish new states for all enabled sensors
  if (this->count_sensor_)
    this->count_sensor_->publish_state(value.get_count());

  if (this->covariance_sensor_) {
    double covariance_ms = value.compute_covariance(this->is_time_weighted_(), this->group_type_);
    double converted_covariance = covariance_ms / this->time_conversion_factor_;

    this->covariance_sensor_->publish_state(converted_covariance);
  }

  if (this->duration_sensor_)
    this->duration_sensor_->publish_state(value.get_duration());

  if (this->max_sensor_) {
    float max = value.get_max();
    if (std::isinf(max)) {  // default aggregated max for 0 measuremnts is -infinity, switch to NaN for HA
      this->max_sensor_->publish_state(NAN);
    } else {
      this->max_sensor_->publish_state(max);
    }
  }

  if (this->mean_sensor_)
    this->mean_sensor_->publish_state(value.get_mean());

  if (this->min_sensor_) {
    float min = value.get_min();
    if (std::isinf(min)) {  // default aggregated min for 0 measurements is infinity, switch to NaN for HA
      this->min_sensor_->publish_state(NAN);
    } else {
      this->min_sensor_->publish_state(min);
    }
  }

  if (this->std_dev_sensor_)
    this->std_dev_sensor_->publish_state(value.compute_std_dev(this->is_time_weighted_(), this->group_type_));

  if (this->trend_sensor_) {
    double trend_ms = value.compute_trend();
    double converted_trend = trend_ms * this->time_conversion_factor_;

    this->trend_sensor_->publish_state(converted_trend);
  }

  if (this->variance_sensor_)
    this->variance_sensor_->publish_state(value.compute_variance(this->is_time_weighted_(), this->group_type_));

  // If saving to flash is enabled, do so
  if (this->restore_)
    this->pref_.save(&value);
}

inline bool StatisticsComponent::is_time_weighted_() { return (this->average_type_ == TIME_WEIGHTED_AVERAGE); }

inline bool StatisticsComponent::is_running_chunk_ready_() {
  // If the chunk_size_ == 0, then our running chunk resets based on a duration
  if (this->chunk_size_ > 0) {
    if (this->running_chunk_count_ >= this->chunk_size_) {
      return true;
    }
  } else {
    if (this->running_chunk_duration_ >= this->chunk_duration_) {
      if (this->running_chunk_count_ > 0)  // ensure we have aggregated at least one measurement in this timespan
        return true;
    }
  }

  return false;
}

}  // namespace statistics
}  // namespace esphome
