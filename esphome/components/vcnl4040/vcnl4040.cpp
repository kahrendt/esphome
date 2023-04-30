/*
 *
 */

#include "vcnl4040.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace vcnl4040 {

static const char *const TAG = "vcnl4040";

void VCNL4040::dump_config() {
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

  ESP_LOGCONFIG(TAG, "  Ambient Interrupt Raw Lower Threshold: %u", this->read_sensor_without_stop_(VCNL4040_ALS_THDL));
  ESP_LOGCONFIG(TAG, "  Ambient Interrupt Raw Upper Threshold: %u", this->read_sensor_without_stop_(VCNL4040_ALS_THDH));
  ESP_LOGCONFIG(TAG, "  Proximity Interrupt Raw Lower Threshold: %u",
                this->read_sensor_without_stop_(VCNL4040_PS_THDL));
  ESP_LOGCONFIG(TAG, "  Proximity Interrupt Raw Upper Threshold: %u",
                this->read_sensor_without_stop_(VCNL4040_PS_THDH));
}

void VCNL4040::setup() {
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

  ////
  // Setup ambient light sensor
  ////

  if (this->lux_sensor_) {
    this->als_conf_.bit.als_sd = 0;  // enable ambient light sensor
  }

  if ((this->bright_event_binary_sensor_) || (this->dark_event_binary_sensor_)) {
    this->als_conf_.bit.als_sd = 0;      // enable ambient light sensor
    this->als_conf_.bit.als_int_en = 1;  // enable interrupt

    // uint16_t als_lower_threshold = this->convert_lux_to_level_(this->ambient_interrupt_lower_bound_);
    // uint16_t als_upper_threshold = this->convert_lux_to_level_(this->ambient_interrupt_upper_bound_);

    this->write_threshold_(VCNL4040_ALS_THDL, this->ambient_interrupt_lower_bound_);
    this->write_threshold_(VCNL4040_ALS_THDH, this->ambient_interrupt_upper_bound_);
  }

  // this->als_conf_.bit.als_it = this->als_integration_time_;   // set configured integration time

  if (!write_als_config_settings_()) {
    ESP_LOGE(TAG, "Failed to write ambient light sensor configuration");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  ////
  // Setup proximity sensor
  ////

  if (this->proximity_sensor_) {
    this->ps_conf1_.bit.ps_sd = 0;  // enable proximity sensor
  }

  if ((this->far_event_binary_sensor_) || (this->close_event_binary_sensor_)) {
    this->ps_conf1_.bit.ps_sd = 0;  // enable proximity sensor

    // enable close event interrupt
    if (this->close_event_binary_sensor_) {
      this->ps_conf2_.bit.ps_int = 0x1 | this->ps_conf2_.bit.ps_int;
    }

    // enable far event interrupt
    if (this->far_event_binary_sensor_) {
      this->ps_conf2_.bit.ps_int = 0x2 | this->ps_conf2_.bit.ps_int;
    }

    // this->ps_conf1_.bit.ps_pers = 0x3;
    // this->ps_conf3_.bit.ps_mps = 0x2;

    // this->ps_conf3_.bit.ps_smart_pers = 0x1; // enable smart persistence

    // this->ps_ms_.bit.ps_ms = 0x0;   // normal PS interrupt mode

    // this->ps_ms_.bit.white_en = 0x1;  // disable white channel

    // this->ps_conf3_.bit.ps_sc_en = 0x1; // enable sunlight cancellation

    // uint16_t proximity_upper_threshold =
    // this->convert_percentage_to_level_(this->proximity_far_event_upper_bound_percentage_); uint16_t
    // proximity_lower_threshold =
    // this->convert_percentage_to_level_(this->proximity_close_event_lower_bound_percentage_);

    this->write_threshold_(VCNL4040_PS_THDL, this->proximity_close_event_lower_bound_);
    this->write_threshold_(VCNL4040_PS_THDH, this->proximity_far_event_upper_bound_);
  }

  // this->ps_conf1_.bit.ps_duty = this->ired_duty_;
  // this->ps_conf1_.bit.ps_it = this->proximity_integration_time_;
  // this->ps_conf2_.bit.ps_hd = this->proximity_output_resolution_;  // enable HD mode for proximity reading

  if (!write_ps_config_settings_()) {
    ESP_LOGE(TAG, "Failed to write proximity sensor configuration");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }
}

void VCNL4040::loop() {
  if ((!this->bright_event_binary_sensor_) && (!this->dark_event_binary_sensor_) && (!this->far_event_binary_sensor_) &&
      (!this->close_event_binary_sensor_)) {
    return;
  } else {
    uint16_t interrupt_info = this->read_sensor_without_stop_(VCNL4040_INT);
    this->int_flag_.reg = (uint8_t) ((0xFF00 & interrupt_info) >> 8);

    if (this->bright_event_binary_sensor_) {
      this->bright_event_binary_sensor_->publish_state(this->int_flag_.bit.als_if_h);
    }

    if (this->dark_event_binary_sensor_) {
      this->dark_event_binary_sensor_->publish_state(this->int_flag_.bit.als_if_l);
    }

    if (this->close_event_binary_sensor_) {
      this->close_event_binary_sensor_->publish_state(this->int_flag_.bit.ps_if_close);
    }

    if (this->far_event_binary_sensor_) {
      this->far_event_binary_sensor_->publish_state(this->int_flag_.bit.ps_if_away);
    }
  }
}

void VCNL4040::update() {
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

bool VCNL4040::read_ambient_light_(float &ambient_light) {
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
                           (0.1 / pow(2.0, (float) this->als_conf_.bit.als_it)));

  return true;
}

bool VCNL4040::read_proximity_(float &proximity) {
  // uint8_t data[2];
  // this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false);
  // // if (!this->read_register(VCNL4040_PS_OUTPUT, &data[0], 2, false)) {
  // //   ESP_LOGE(TAG, "Failed to read sensor's ambient light data");
  // //   this->status_set_warning();

  // //   return false;
  // // }
  // ESP_LOGD(TAG, "raw proximity lsb = %d, msb = %d", data[0], data[1]);
  // uint16_t raw_data = (int16_t) data[1] << 8 | (int16_t) data[0];
  float maximum = (this->ps_conf2_.bit.ps_hd == PS_RESOLUTION_12 ? pow(2.0, 12.0) : pow(2.0, 16.0));
  float raw_reading = (float) this->read_sensor_without_stop_(VCNL4040_PS_OUTPUT);

  proximity = 100.0 * raw_reading / maximum;

  return true;
}

bool VCNL4040::read_white_channel_(float &white_channel) {
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

uint16_t VCNL4040::read_sensor_without_stop_(uint8_t register_address) {
  uint8_t data[2];
  this->read_register(register_address, &data[0], 2, false);  // read without sending a stop
  return ((uint16_t) data[1] << 8 | (uint16_t) data[0]);
}

bool VCNL4040::write_als_config_settings_() {
  this->write_lsb_and_msb_(VCNL4040_ALS_CONF, this->als_conf_.reg, 0x00);

  uint8_t verify_data[2];
  this->read_register(VCNL4040_ALS_CONF, &verify_data[0], 2, false);
  if ((verify_data[0] != this->als_conf_.reg) || (verify_data[1] != 0x00))
    return false;
  else
    return true;
}

bool VCNL4040::write_ps_config_settings_() {
  this->write_lsb_and_msb_(VCNL4040_PS_CONF_FIRST, this->ps_conf1_.reg, this->ps_conf2_.reg);

  uint8_t verify_data[2];
  this->read_register(VCNL4040_PS_CONF_FIRST, &verify_data[0], 2, false);
  if ((verify_data[0] != this->ps_conf1_.reg) || (verify_data[1] != this->ps_conf2_.reg))
    return false;

  this->write_lsb_and_msb_(VCNL4040_PS_CONF_LAST, this->ps_conf3_.reg, this->ps_ms_.reg);

  this->read_register(VCNL4040_PS_CONF_LAST, &verify_data[0], 2, false);
  if ((verify_data[0] != this->ps_conf3_.reg) || (verify_data[1] != this->ps_ms_.reg))
    return false;
  else
    return true;
}

bool VCNL4040::write_lsb_and_msb_(uint8_t address, uint8_t lsb, uint8_t msb) {
  uint8_t write_data[2];
  write_data[0] = lsb;
  write_data[1] = msb;

  return this->write_register(address, &write_data[0], 2, true);
}

bool VCNL4040::write_threshold_(uint8_t address, uint16_t threshold) {
  uint8_t lsb = (uint8_t) (threshold & 0xFF);
  uint8_t msb = (uint8_t) ((0xFF00 & threshold) >> 8);
  return this->write_lsb_and_msb_(address, lsb, msb);
}

}  // namespace vcnl4040
}  // namespace esphome
