/*
 *
 */

#include "vcnl4040.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace vcnl4040 {

static const char *const TAG = "vcnl4040";

void VCNL4040Component::dump_config() {
  ESP_LOGCONFIG(TAG, "VCNL4040:");

  switch (this->error_code_) {
    case NONE:
      break;
    case ERROR_COMMUNICATION_FAILED:
      ESP_LOGE(TAG, "  Communication with VCNL4040 failed!");
      break;
    case ERROR_WRONG_CHIP_ID:
      ESP_LOGE(TAG, "  VCNL4040 has wrong chip ID - please verify you are using a VCNL4040");
      break;
  }

  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);

  if (this->lux_sensor_) {
    LOG_SENSOR("  ", "Lux", this->lux_sensor_);
  }

  if (this->proximity_sensor_) {
    LOG_SENSOR("  ", "Proximity", this->proximity_sensor_);
  }
}

void VCNL4040Component::setup() {
  this->error_code_ = NONE;
  ESP_LOGCONFIG(TAG, "Setting up VCNL4040...");

  ///////////////////////////////////////////
  // 1) Verify chip ID //
  ///////////////////////////////////////////

  uint8_t chip_id[2];

  // read chip id from sensor
  this->read_register(VCNL4040_ID, &chip_id[0], 2, false);

  // verify id
  if (chip_id[0] != VCNL4040_CHIP_ID) {
    ESP_LOGE(TAG, "Unknown chip ID, is this a VCNL4040?");

    this->error_code_ = ERROR_WRONG_CHIP_ID;
    this->mark_failed();

    return;
  }

  //////
  // 2) Setup ambient light sensor (if applicable)
  /////
  if (this->lux_sensor_) {
    this->als_conf_.bit.als_sd = 0;  // enable ambient light sensor
    this->als_conf_.bit.als_it = this->als_integration_time_;

    if (!write_als_config_settings_()) {
      ESP_LOGE(TAG, "Failed to write ambient light sensor configuration");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }
  }

  /////
  // 3) Setup proximity sensor (if applicable)
  /////

  if (this->proximity_sensor_) {
    this->ps_conf1_.bit.ps_sd = 0;  // enable proximity sensor
    this->ps_conf1_.bit.ps_duty = this->ired_duty_;
    this->ps_conf1_.bit.ps_it = this->proximity_integration_time_;
    this->ps_conf2_.bit.ps_hd = this->proximity_output_resolution_;  // enable HD mode for proximity reading

    if (!write_ps_config_settings_()) {
      ESP_LOGE(TAG, "Failed to write proximity sensor configuration");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }
  }
}

void VCNL4040Component::update() {
  ////////////////////////////////////////////////////////////////////////////////////
  // 0) Verify either a lux or proximity sensor is defined before proceeding //
  ////////////////////////////////////////////////////////////////////////////////////

  if ((!this->lux_sensor_) && (!this->proximity_sensor_)) {
    return;
  }

  if (this->lux_sensor_) {
    float lux_reading;

    if (!this->read_ambient_light_(lux_reading)) {
      ESP_LOGE(TAG, "Failed to read ambient light measurement, skipping update");
      this->status_set_warning();

      return;
    }

    this->lux_sensor_->publish_state(lux_reading);
  }

  if (this->proximity_sensor_) {
    float proximity_reading;

    if (!this->read_proximity_(proximity_reading)) {
      ESP_LOGE(TAG, "Failed to read proximity measurement, skipping update");
      this->status_set_warning();

      return;
    }

    this->proximity_sensor_->publish_state(proximity_reading);
  }

  if (this->white_channel_sensor_) {
    float white_channel_reading;

    if (!this->read_white_channel_(white_channel_reading)) {
      ESP_LOGE(TAG, "Failed to read proximity measurement, skipping update");
      this->status_set_warning();

      return;
    }

    this->white_channel_sensor_->publish_state(white_channel_reading);
  }
}

bool VCNL4040Component::read_ambient_light_(float &ambient_light) {
  // see datasheet page 12 for formula to scale reading to lux based on the configured integration time

  // uint8_t data[2];
  // this->read_register(VCNL4040_ALS_OUTPUT, &data[0], 2, false);
  // // if (!this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false)) {
  // //   ESP_LOGE(TAG, "Failed to read sensor's ambient light data");
  // //   this->status_set_warning();

  // //   return false;
  // // }
  // uint16_t raw_data = (int16_t) data[1] << 8 | (int16_t) data[0];

  ambient_light = (float) ((float) this->read_sensor_without_stop_(VCNL4040_ALS_OUTPUT) *
                           (0.1 / pow(2.0, (float) this->als_integration_time_)));

  return true;
}

bool VCNL4040Component::read_proximity_(float &proximity) {
  // uint8_t data[2];
  // this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false);
  // // if (!this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false)) {
  // //   ESP_LOGE(TAG, "Failed to read sensor's ambient light data");
  // //   this->status_set_warning();

  // //   return false;
  // // }
  // ESP_LOGD(TAG, "raw proximity lsb = %d, msb = %d", data[0], data[1]);
  // uint16_t raw_data = (int16_t) data[1] << 8 | (int16_t) data[0];

  proximity = (float) this->read_sensor_without_stop_(VCNL4040_PS_OUTPUT);
  return true;
}

bool VCNL4040Component::read_white_channel_(float &white_channel) {
  // uint8_t data[2];
  // this->read_register(VCNL4040_WHITE_OUTPUT, &data[0], 2, false);
  // // if (!this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false)) {
  // //   ESP_LOGE(TAG, "Failed to read sensor's ambient light data");
  // //   this->status_set_warning();

  // //   return false;
  // // }
  // ESP_LOGD(TAG, "raw white channel lsb = %d, msb = %d", data[0], data[1]);
  // uint16_t raw_data = (int16_t) data[1] << 8 | (int16_t) data[0];

  white_channel = (float) this->read_sensor_without_stop_(VCNL4040_WHITE_OUTPUT);

  return true;
}

uint16_t VCNL4040Component::read_sensor_without_stop_(uint8_t register_address) {
  uint8_t data[2];
  this->read_register(register_address, &data[0], 2, false);  // read without sending a stop
  return ((uint16_t) data[1] << 8 | (uint16_t) data[0]);
}

bool VCNL4040Component::write_als_config_settings_() {
  uint8_t write_data[2];
  write_data[0] = this->als_conf_.reg;
  write_data[1] = 0x00;
  this->write_register(VCNL4040_ALS_CONF, &write_data[0], 2, true);

  uint8_t verify_data[2];
  this->read_register(VCNL4040_ALS_CONF, &verify_data[0], 2, false);
  if ((verify_data[0] != write_data[0]) || (verify_data[1] != write_data[1]))
    return false;
  else
    return true;
}

bool VCNL4040Component::write_ps_config_settings_() {
  uint8_t write_data[2];
  write_data[0] = this->ps_conf1_.reg;
  write_data[1] = this->ps_conf2_.reg;
  this->write_register(VCNL4040_PS_CONF_FIRST, &write_data[0], 2, true);

  uint8_t verify_data[2];
  this->read_register(VCNL4040_PS_CONF_FIRST, &verify_data[0], 2, false);
  if ((verify_data[0] != write_data[0]) || (verify_data[1] != write_data[1]))
    return false;

  write_data[0] = this->ps_conf3_.reg;
  write_data[1] = this->ps_ms_.reg;
  this->write_register(VCNL4040_PS_CONF_LAST, &write_data[0], 2, true);

  this->read_register(VCNL4040_PS_CONF_LAST, &verify_data[0], 2, false);
  if ((verify_data[0] != write_data[0]) || (verify_data[1] != write_data[1]))
    return false;
  else
    return true;
}

}  // namespace vcnl4040
}  // namespace esphome
