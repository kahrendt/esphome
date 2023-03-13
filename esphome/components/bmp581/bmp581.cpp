/*
  Adds support for BMP581 high accuracy pressure and temperature sensor
    - Based on ESPHome's BMP3XX component
      - Implementation is easier as the sensor itself performs the temperature compensation.
    - Bosch's BMP5-Sensor-API was consulted to verify that sensor configuration is done correctly
      - Copyright (c) 2022 Bosch Sensortec Gmbh, SPDX-License-Identifier: BSD-3-Clause
    - This component uses forced power mode only so it follows host synchronization
*/

#include "bmp581.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace bmp581 {

static const char *const TAG = "bmp581";

static const LogString *oversampling_to_str(Oversampling oversampling) {
  switch (oversampling) {
    case Oversampling::OVERSAMPLING_NONE:
      return LOG_STR("None");
    case Oversampling::OVERSAMPLING_X2:
      return LOG_STR("2x");
    case Oversampling::OVERSAMPLING_X4:
      return LOG_STR("4x");
    case Oversampling::OVERSAMPLING_X8:
      return LOG_STR("8x");
    case Oversampling::OVERSAMPLING_X16:
      return LOG_STR("16x");
    case Oversampling::OVERSAMPLING_X32:
      return LOG_STR("32x");
    case Oversampling::OVERSAMPLING_X64:
      return LOG_STR("64x");
    case Oversampling::OVERSAMPLING_X128:
      return LOG_STR("128x");
    default:
      return LOG_STR("");
  }
}

static const LogString *iir_filter_to_str(IIRFilter filter) {
  switch (filter) {
    case IIRFilter::IIR_FILTER_OFF:
      return LOG_STR("OFF");
    case IIRFilter::IIR_FILTER_2:
      return LOG_STR("2x");
    case IIRFilter::IIR_FILTER_4:
      return LOG_STR("4x");
    case IIRFilter::IIR_FILTER_8:
      return LOG_STR("8x");
    case IIRFilter::IIR_FILTER_16:
      return LOG_STR("16x");
    case IIRFilter::IIR_FILTER_32:
      return LOG_STR("32x");
    case IIRFilter::IIR_FILTER_64:
      return LOG_STR("64x");
    case IIRFilter::IIR_FILTER_128:
      return LOG_STR("128x");
    default:
      return LOG_STR("");
  }
}

void BMP581Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMP581:");

  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);

  if (this->temperature_sensor_) {
    LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
    ESP_LOGCONFIG(TAG, "    IIR Temperature Filter: %s", LOG_STR_ARG(iir_filter_to_str(this->iir_temperature_level_)));
    ESP_LOGCONFIG(TAG, "    Temperature Oversampling: %s",
                  LOG_STR_ARG(oversampling_to_str(this->temperature_oversampling_)));
  }

  if (this->pressure_sensor_) {
    LOG_SENSOR("  ", "Pressure", this->pressure_sensor_);
    ESP_LOGCONFIG(TAG, "    IIR Temperature Filter: %s", LOG_STR_ARG(iir_filter_to_str(this->iir_pressure_level_)));
    ESP_LOGCONFIG(TAG, "    Temperature Oversampling: %s",
                  LOG_STR_ARG(oversampling_to_str(this->pressure_oversampling_)));
  }
}

void BMP581Component::setup() {
  /*
  Setup goes through several stages
    1) Soft reboot
    2) Verify ASIC chip ID matches BMP581
    3) Verify sensor status (NVM is okay)
    4) Set output data rate and power  mode
    5) Set oversampling rate
    6) If configured, set IIR filter level
    7) Enable data ready interrupt
  */

  // 1) Soft reboot
  if (!this->reset_()) {
    this->mark_failed();
    return;
  }

  // 2) Verify ASIC chip ID matches BMP581
  if (!this->verify_chip_id_()) {
    this->mark_failed();
    return;
  }

  // 3) Verify sensor status (NVM is okay)
  if (!this->verify_status_()) {
    this->mark_failed();
    return;
  }

  // 4) Set output data rate and power  mode
  this->odr_config_.bit.pwr_mode = STANDBY_MODE;  // standby mode to start
  this->odr_config_.bit.odr =
      0x19;  // 4 Hz data out rate; shouldn't matter as we are in forced mode (though allows deep standby?)

  if (!this->set_odr_register_(this->odr_config_.reg)) {
    this->mark_failed();
    return;
  }
  delay(4);

  // 5) Set oversampling rate
  // disable pressure readings and oversampling if no sensor is defined, otherwise set up appropriately
  if (this->pressure_sensor_ == nullptr) {
    this->osr_config_.bit.osr_p = OVERSAMPLING_NONE;
    this->osr_config_.bit.press_en = false;
  } else {
    this->osr_config_.bit.osr_p = this->pressure_oversampling_;
    this->osr_config_.bit.press_en = true;
  }

  // disable temperature oversampling if no sensor is defined (sensor still internally compensates regardless)
  if (this->temperature_sensor_ == nullptr) {
    this->osr_config_.bit.osr_t = OVERSAMPLING_NONE;
  } else {
    this->osr_config_.bit.osr_t = this->temperature_oversampling_;
  }

  // set oversampling register
  if (!this->write_byte(BMP581_OSR, this->osr_config_.reg)) {
    ESP_LOGE(TAG, "Failed to write oversampling register");

    this->mark_failed();
    return;
  }
  delay(4);

  // 6) If configured, set IIR filter level
  if ((this->iir_temperature_level_ != IIR_FILTER_OFF) || (this->iir_pressure_level_ != IIR_FILTER_OFF)) {
    // read in one data point to prime the IIR filter
    this->set_mode_(FORCED_MODE);

    // set IIR level for temperature and pressure
    this->iir_config_.bit.set_iir_t = this->iir_temperature_level_;
    this->iir_config_.bit.set_iir_p = this->iir_pressure_level_;
    if (!this->write_byte(BMP581_DSP_IIR, this->iir_config_.reg)) {
      ESP_LOGE(TAG, "Failed to write IIR configuration register");
      this->mark_failed();
      return;
    }
    delay(4);

    // set data registers to store IIR filtered values if filter is enabled
    this->dsp_config_.bit.shdw_sel_iir_t = (this->iir_temperature_level_ != IIR_FILTER_OFF);
    this->dsp_config_.bit.shdw_sel_iir_p = (this->iir_pressure_level_ != IIR_FILTER_OFF);

    this->dsp_config_.bit.comp_pt_en = 0x3;  // enable pressure temperature compensation

    if (!this->write_byte(BMP581_DSP, this->dsp_config_.reg)) {
      ESP_LOGE(TAG, "Failed to write data register's IIR source register");
      this->mark_failed();
      return;
    }
    delay(4);
  }

  // 7) Enable data ready interrupt
  this->int_source_.bit.drdy_data_reg_en = 0x1;  // enable data ready interrupt

  if (!this->write_byte(BMP581_INT_SOURCE, this->int_source_.reg)) {
    ESP_LOGE(TAG, "Failed to set interrupt enable register");
    this->mark_failed();
    return;
  }
}

void BMP581Component::update() {
  /*
  Each update goes through several stages
    0) Verify a temperature or pressure sensor is defined befor proceeding
    1) Set forced power mode to request sensor readings
    2) Verify sensor has data ready
    3) Read data registers for temperature and pressure if applicable
    4) Compute and publish pressure if sensor is defined
    5) Compute and publish temmperature if sensor is defined
  */

  // 0) Verify a temperature or pressure sensor is defined before proceeding
  if ((this->temperature_sensor_ == nullptr) && (this->pressure_sensor_ == nullptr)) {
    return;
  }

  // 1) Set forced power mode to request sensor readings
  if (!this->set_mode_(FORCED_MODE)) {
    ESP_LOGD(TAG, "Forced reading request failed");
    return;
  }

  // 2) Verify sensor has data ready
  if (!this->get_data_ready_status_()) {
    ESP_LOGD(TAG, "Data isn't ready, skipping update.");
    return;
  }

  // 3) Read data registers for temperature and pressure if applicable
  uint8_t data[6];

  // only read 3 bytes of temperature data if pressure sensor is not defined
  if (this->pressure_sensor_ == nullptr) {
    if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], 3)) {
      ESP_LOGE(TAG, "Failed to read sensor data");
      return;
    }
  }
  // read 6 bytes of temperature and pressure data if pressure sensor is defined
  else {
    if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], 6)) {
      ESP_LOGE(TAG, "Failed to read sensor data");
      return;
    }

    // 4) Compute and publish pressure if sensor is defined
    int32_t raw_press = (int32_t) data[5] << 16 | (int32_t) data[4] << 8 | (int32_t) data[3];
    float pressure = (float) ((raw_press / 64.0) / 100.0);  // Divide by 2^6=64 for Pa, divide by 100 to get hPA

    this->pressure_sensor_->publish_state(pressure);
  }

  // 5) Compute and publish temmperature if sensor is defined
  if (this->temperature_sensor_) {
    int32_t raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
    float temperature = (float) (raw_temp / 65536.0);

    this->temperature_sensor_->publish_state(temperature);
  }
}

// returns if the sensor has data ready to be read
bool BMP581Component::get_data_ready_status_() {
  if (this->odr_config_.bit.pwr_mode == STANDBY_MODE) {
    ESP_LOGD(TAG, "Data not ready, sensor is in standby mode");
    return false;
  }

  uint8_t status;

  if (!this->read_byte(BMP581_INT_STATUS, &status)) {
    ESP_LOGE(TAG, "Failed to read interrupt status register");
    return false;
  }

  this->int_status_.reg = status;

  if (this->int_status_.bit.drdy_data_reg) {
    // if in forced mode, the put our record of the power mode to standby
    if (this->odr_config_.bit.pwr_mode == FORCED_MODE) {
      this->odr_config_.bit.pwr_mode = STANDBY_MODE;
    }

    return true;
  }

  return false;
}

// configure output data rate and power mode by writing to ODR register and return success
bool BMP581Component::set_odr_register_(uint8_t reg_value) {
  if (!this->write_byte(BMP581_ODR, reg_value)) {
    ESP_LOGE(TAG, "Failed to write ODR register/power mode");

    return false;
  }

  return true;
}

// get the chip id from device and verify it matches the known id
bool BMP581Component::verify_chip_id_() {
  uint8_t chip_id;
  if (!this->read_byte(BMP581_CHIP_ID, &chip_id))  // read chip id
  {
    ESP_LOGE(TAG, "Failed to read chip id");
    return false;
  }

  if (chip_id != BMP581_ID) {
    ESP_LOGE(TAG, "Unknown chip ID, is this a BMP581?");
    return false;
  }

  return true;
}

// get the status and check for readiness & errors and return success
bool BMP581Component::verify_status_() {
  if (!this->read_byte(BMP581_STATUS, &this->status_.reg)) {
    ESP_LOGE(TAG, "Failed to read status register");

    return false;
  }

  if (!(this->status_.bit.status_nvm_rdy))  // check status_nvm_rdy bit (should be 1 if okay)
  {
    ESP_LOGE(TAG, "NVM not ready");
    return false;
  }

  if (this->status_.bit.status_nvm_err)  // check status_nvm_err bit (should be 0 if okay)
  {
    ESP_LOGE(TAG, "NVM error detected");
    return false;
  }

  return true;
}

// set the power mode on sensor by writing to ODR register and return success
bool BMP581Component::set_mode_(OperationMode mode) {
  this->odr_config_.bit.pwr_mode = mode;
  return this->set_odr_register_(this->odr_config_.reg);
}

// reset the sensor by writing to the command register and return success
bool BMP581Component::reset_() {
  if (!this->write_byte(BMP581_COMMAND, RESET_COMMAND)) {
    ESP_LOGE(TAG, "Failed to write reset command");
    return false;
  }
  delay(4);  // t_{soft_res} is 2ms (page 11 of datasheet))

  if (!this->read_byte(BMP581_INT_STATUS, &this->int_status_.reg)) {
    ESP_LOGE(TAG, "Failed to read interrupt status register");
    return false;
  }

  // POR bit returns if reboot was successful or not
  return this->int_status_.bit.por;
}

}  // namespace bmp581
}  // namespace esphome
