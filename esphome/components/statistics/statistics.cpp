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
  l_ = q_.begin();
  r_ = q_.begin();
  a_ = q_.begin();
  b_ = q_.begin();

  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}


void StatisticsComponent::handle_new_value_(float value) {
  while (size() >= this->window_size_) {
    evict();
  }
  insert(value);

  if (++this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;

    Partial summary = query();

    if (this->mean_sensor_) {
      this->mean_sensor_->publish_state(lower_mean(summary));
    }

    if (this->max_sensor_) {
      this->max_sensor_->publish_state(lower_max(summary));
    }

    if (this->min_sensor_) {
      this->min_sensor_->publish_state(lower_min(summary));
    }

    if (this->sd_sensor_) {
      this->sd_sensor_->publish_state(lower_sd(summary));
    }
  }
}

}  // namespace statistics
}  // namespace esphome
