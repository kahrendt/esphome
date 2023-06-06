/*
 *
 */

#include "statistics.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <numeric>
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
}

void StatisticsComponent::setup() {
  this->queue_.set_window_size(this->window_size_);

  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}

void StatisticsComponent::update_current_statistics_() { this->current_statistics_ = this->queue_.query(); }

void StatisticsComponent::handle_new_value_(float value) {
  while (this->queue_.size() >= this->window_size_) {
    this->queue_.evict();
  }
  this->queue_.insert(value);

  if (++this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    this->update_current_statistics_();

    if (this->mean_sensor_) {
      this->mean_sensor_->publish_state(this->mean_());
    }

    if (this->max_sensor_) {
      this->max_sensor_->publish_state(this->max_());
    }

    if (this->min_sensor_) {
      this->min_sensor_->publish_state(this->min_());
    }

    if (this->sd_sensor_) {
      this->sd_sensor_->publish_state(this->sd_());
    }
  }
}

}  // namespace statistics
}  // namespace esphome
