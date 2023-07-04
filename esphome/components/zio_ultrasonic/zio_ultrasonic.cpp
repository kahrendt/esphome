
#include "zio_ultrasonic.h"

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/log.h"

namespace esphome {
namespace zio_ultrasonic {

static const char *const TAG = "Zio Ultrasonic";

void ZioUltrasonicComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Zio Ultrasonic:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Distance", this);
}

void ZioUltrasonicComponent::update() {
  uint16_t distance;

  if (!this->read_byte_16(0x01, &distance)) {
    ESP_LOGE(TAG, "Error reading data from ZioUltrasonic");
  } else {
    this->publish_state(distance);
  }
}

}  // namespace zio_ultrasonic
}  // namespace esphome
