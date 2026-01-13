#include "es9018k2m.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace es9018k2m {

static const char *const TAG = "es9018k2m";

void ES9018K2M::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ES9018K2M...");

  // Set initial volume (full volume)
  if (!this->write_volume_()) {
    ESP_LOGE(TAG, "Failed to set initial volume");
    this->mark_failed();
    return;
  }

  // Ensure unmuted at startup
  if (!this->write_mute_()) {
    ESP_LOGE(TAG, "Failed to set initial mute state");
    this->mark_failed();
    return;
  }
}

void ES9018K2M::dump_config() {
  ESP_LOGCONFIG(TAG, "ES9018K2M:");
  LOG_I2C_DEVICE(this);

  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication failed");
  }
}

bool ES9018K2M::set_mute_off() {
  this->is_muted_ = false;
  return this->write_mute_();
}

bool ES9018K2M::set_mute_on() {
  this->is_muted_ = true;
  return this->write_mute_();
}

bool ES9018K2M::set_volume(float volume) {
  this->volume_ = clamp<float>(volume, 0.0f, 1.0f);
  return this->write_volume_();
}

bool ES9018K2M::is_muted() { return this->is_muted_; }

float ES9018K2M::volume() { return this->volume_; }

bool ES9018K2M::write_mute_() {
  // Register 7 (General Settings):
  // - Bit 7 must remain 1 (reserved)
  // - Bits 6:5 = filter_shape (leave as default 0)
  // - Bit 4 = reserved (leave as 0)
  // - Bits 3:2 = iir_bw (leave as default 0)
  // - Bit 1 = mute channel 2 (right)
  // - Bit 0 = mute channel 1 (left)
  // Default value is 0x80
  uint8_t reg_value = 0x80;  // Preserve reserved bit 7
  if (this->is_muted_) {
    reg_value |= 0x03;  // Set both mute bits
  }

  if (!this->write_byte(ES9018K2M_REG_GENERAL_SETTINGS, reg_value)) {
    ESP_LOGE(TAG, "Failed to write mute state");
    return false;
  }
  return true;
}

bool ES9018K2M::write_volume_() {
  // ES9018K2M volume registers:
  // - 0 = 0dB (full volume)
  // - 255 = -127.5dB (minimum volume)
  // Each step = 0.5dB attenuation
  //
  // ESPHome volume: 0.0 (silent) to 1.0 (full volume)
  // Conversion: register_value = (1.0 - volume) * 255
  uint8_t volume_byte = static_cast<uint8_t>((1.0f - this->volume_) * 255.0f);

  ESP_LOGD(TAG, "Setting volume to %.2f (register: 0x%02X)", this->volume_, volume_byte);

  // Write to both left and right channel volume registers
  if (!this->write_byte(ES9018K2M_REG_VOLUME_1, volume_byte) ||
      !this->write_byte(ES9018K2M_REG_VOLUME_2, volume_byte)) {
    ESP_LOGE(TAG, "Failed to write volume");
    return false;
  }
  return true;
}

}  // namespace es9018k2m
}  // namespace esphome
