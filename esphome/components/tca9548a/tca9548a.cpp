#include "tca9548a.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tca9548a {

static const char *const TAG = "tca9548a";

i2c::ErrorCode TCA9548AChannel::readv(uint8_t address, i2c::ReadBuffer *buffers, size_t cnt) {
  auto err = this->parent_->switch_to_channel(channel_);
  if (err != i2c::ERROR_OK)
    return err;
  err = this->parent_->bus_->readv(address, buffers, cnt);
  if (this->parent_->get_multiple_tca9548a())  // if there are other tca9548a's, then disable all channels
    this->parent_->disable_all_channels();
  return err;
}
i2c::ErrorCode TCA9548AChannel::writev(uint8_t address, i2c::WriteBuffer *buffers, size_t cnt, bool stop) {
  auto err = this->parent_->switch_to_channel(channel_);
  if (err != i2c::ERROR_OK)
    return err;
  err = this->parent_->bus_->writev(address, buffers, cnt, stop);
  if (this->parent_->get_multiple_tca9548a())  // if there are other tca9548a's, then disable all channels
    this->parent_->disable_all_channels();
  return err;
}

void TCA9548AComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up TCA9548A...");
  uint8_t status = 0;
  if (this->read(&status, 1) != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "TCA9548A failed");
    this->mark_failed();
    return;
  }
  ESP_LOGD(TAG, "Channels currently open: %d", status);
}
void TCA9548AComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "TCA9548A:");
  LOG_I2C_DEVICE(this);
}

i2c::ErrorCode TCA9548AComponent::switch_to_channel(uint8_t channel) {
  if (this->is_failed())
    return i2c::ERROR_NOT_INITIALIZED;
  if (current_channel_ == channel)
    return i2c::ERROR_OK;

  uint8_t channel_val = 1 << channel;
  auto err = this->write(&channel_val, 1);
  if (err == i2c::ERROR_OK) {
    this->current_channel_ = channel;
  }
  return err;
}

void TCA9548AComponent::disable_all_channels() {
  if (this->is_failed())
    return;

  const uint8_t null_channel = 0;
  auto err = this->write(&null_channel, 1);

  if (err == i2c::ERROR_OK) {
    this->current_channel_ = 255;  // no channels are enabled, so set current_channel_ to default
  } else {
    this->mark_failed();  // failed to disable channels, mark entire component failed to avoid address conflicts
    ESP_LOGE(TAG, "Failed to disable all channels.");
  }
}

}  // namespace tca9548a
}  // namespace esphome
