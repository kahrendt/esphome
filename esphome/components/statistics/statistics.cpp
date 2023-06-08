/*
 *
 */

#include "statistics.h"
#include "daba_lite.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

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

// void StatisticsComponent::update_current_statistics_() {
//   this->current_statistics_ = this->partial_stats_queue_.query();
// }

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
      if (std::isinf(max))  // default aggregated max for 0 readings is -infinity, switch to NaN for HA
        this->max_sensor_->publish_state(NAN);
      else
        this->max_sensor_->publish_state(max);
    }

    if (this->min_sensor_) {
      float min = this->partial_stats_queue_.aggregated_min();
      if (std::isinf(min))  // default aggregated min for 0 readings is infinity, switch to NaN for HA
        this->max_sensor_->publish_state(NAN);
      else
        this->max_sensor_->publish_state(min);
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
