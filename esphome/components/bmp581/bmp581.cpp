/*
 * Adds support for BMP581 high accuracy pressure and temperature sensor
 *  - Component structure based on ESPHome's BMP3XX component
 *    - Implementation is easier as the sensor itself performs the temperature compensation
 *      - Temperature and pressure data is converted via simple divison operations
 *    - IIR filter level can be applied to temperature and pressure sensors independently
 *  - Bosch's BMP5-Sensor-API was consulted to verify that sensor configuration is done correctly
 *    - Copyright (c) 2022 Bosch Sensortec Gmbh, SPDX-License-Identifier: BSD-3-Clause
 *  - This component uses forced power mode only so measurements follow host synchronization
 *  - All datasheet page references refer to Bosch Document Number BST-BMP581-DS004-04 (revision number 1.4)
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

  switch (this->error_code_) {
    case NONE:
      break;
    case ERROR_COMMUNICATION_FAILED:
      ESP_LOGE(TAG, "  Communication with BM581 failed!");
      break;
    case ERROR_WRONG_CHIP_ID:
      ESP_LOGE(TAG, "  BMP581 has wrong chip ID - please verify you are using a BMP 581");
      break;
    case ERROR_SENSOR_RESET:
      ESP_LOGE(TAG, "  BMP581 failed to reset");
      break;
    case ERROR_SENSOR_STATUS:
      ESP_LOGE(TAG, "  BMP581 sensor status failed, there were NVM problems");
      break;
    default:
      ESP_LOGE(TAG, "  BMP581 error code %d", (int) this->error_code_);
      break;
  }

  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);

  if (this->temperature_sensor_) {
    LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);
    ESP_LOGCONFIG(TAG, "    IIR Filter: %s", LOG_STR_ARG(iir_filter_to_str(this->iir_temperature_level_)));
    ESP_LOGCONFIG(TAG, "    Oversampling: %s", LOG_STR_ARG(oversampling_to_str(this->temperature_oversampling_)));
  }

  if (this->pressure_sensor_) {
    LOG_SENSOR("  ", "Pressure", this->pressure_sensor_);
    ESP_LOGCONFIG(TAG, "    IIR Filter: %s", LOG_STR_ARG(iir_filter_to_str(this->iir_pressure_level_)));
    ESP_LOGCONFIG(TAG, "    Oversampling: %s", LOG_STR_ARG(oversampling_to_str(this->pressure_oversampling_)));
  }
}

void BMP581Component::setup() {
  /*
   * Setup goes through several stages, which follows the post-power-up procedure (page 18 of datasheet) and then sets
   * configuration options
   *  1) Soft reboot
   *  2) Verify ASIC chip ID matches BMP581
   *  3) Verify sensor status (check if NVM is okay)
   *  4) Set output data rate and power mode
   *  5) Set oversampling rate
   *  6) If configured, set IIR filter level
   *  7) Enable data ready interrupt
   */

  this->error_code_ = NONE;
  ESP_LOGCONFIG(TAG, "Setting up BMP581...");

  ////////////////////
  // 1) Soft reboot //
  ////////////////////

  // writes reset command to BMP's command register
  if (!this->write_byte(BMP581_COMMAND, RESET_COMMAND)) {
    ESP_LOGE(TAG, "Failed to write reset command");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  delay(4);  // t_{soft_res} is 2ms, so delay to ensure enough time for full reboot (page 11 of datasheet))

  // read interrupt status register
  if (!this->read_byte(BMP581_INT_STATUS, &this->int_status_.reg)) {
    ESP_LOGE(TAG, "Failed to read interrupt status register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  // Power-On-Reboot bit is asserted if sensor successfully reset
  if (!this->int_status_.bit.por) {
    ESP_LOGE(TAG, "BMP581 failed to reset");

    this->error_code_ = ERROR_SENSOR_RESET;
    this->mark_failed();

    return;
  }

  ///////////////////////////////////////////
  // 2) Verify ASIC chip ID matches BMP581 //
  ///////////////////////////////////////////

  uint8_t chip_id;
  if (!this->read_byte(BMP581_CHIP_ID, &chip_id))  // read chip id
  {
    ESP_LOGE(TAG, "Failed to read chip id");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  if (chip_id != BMP581_ID) {
    ESP_LOGE(TAG, "Unknown chip ID, is this a BMP581?");

    this->error_code_ = ERROR_WRONG_CHIP_ID;
    this->mark_failed();

    return;
  }

  ////////////////////////////////////////////////////
  // 3) Verify sensor status (check if NVM is okay) //
  ////////////////////////////////////////////////////

  if (!this->read_byte(BMP581_STATUS, &this->status_.reg)) {
    ESP_LOGE(TAG, "Failed to read status register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  if (!(this->status_.bit.status_nvm_rdy))  // check status_nvm_rdy bit (should be 1 if okay)
  {
    ESP_LOGE(TAG, "NVM not ready after boot");

    this->error_code_ = ERROR_SENSOR_STATUS;
    this->mark_failed();

    return;
  }

  if (this->status_.bit.status_nvm_err)  // check status_nvm_err bit (should be 0 if okay)
  {
    ESP_LOGE(TAG, "NVM error detected on boot");

    this->error_code_ = ERROR_SENSOR_STATUS;
    this->mark_failed();

    return;
  }

  ////////////////////////////////////////////
  // 4) Set output data rate and power mode //
  ////////////////////////////////////////////

  this->odr_config_.bit.pwr_mode = STANDBY_MODE;  // standby mode to start
  this->odr_config_.bit.odr = 0x19;  // 4 Hz output data rate; shouldn't matter as we are in forced mode (should still
                                     // allow deep standby for powersaving)

  // write odr register
  if (!this->write_byte(BMP581_ODR, this->odr_config_.reg)) {
    ESP_LOGE(TAG, "Failed to write ODR register/power mode");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  //////////////////////////////
  // 5) Set oversampling rate //
  //////////////////////////////

  // disable pressure readings and oversampling if no pressure sensor is defined, otherwise set up appropriately
  if (this->pressure_sensor_ == nullptr) {
    this->osr_config_.bit.osr_p = OVERSAMPLING_NONE;
    this->osr_config_.bit.press_en = false;
  } else {
    this->osr_config_.bit.osr_p = this->pressure_oversampling_;
    this->osr_config_.bit.press_en = true;
  }

  // disable temperature oversampling if no temperature sensor is defined (sensor will still internally compensate for
  // temperature)
  if (this->temperature_sensor_ == nullptr) {
    this->osr_config_.bit.osr_t = OVERSAMPLING_NONE;
  } else {
    this->osr_config_.bit.osr_t = this->temperature_oversampling_;
  }

  // write oversampling register
  if (!this->write_byte(BMP581_OSR, this->osr_config_.reg)) {
    ESP_LOGE(TAG, "Failed to write oversampling register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  ////////////////////////////////////////////
  // 6) If configured, set IIR filter level //
  ////////////////////////////////////////////

  if ((this->iir_temperature_level_ != IIR_FILTER_OFF) || (this->iir_pressure_level_ != IIR_FILTER_OFF)) {
    // disable deep standby mode which clears IIR filter
    this->odr_config_.bit.deep_dis = true;

    // read in one data point to prime the IIR filter, otherwise its first previous value is 0 for both sensors
    // also pushes the deep standby disable configuration
    if (!this->set_power_mode_(FORCED_MODE)) {
      ESP_LOGE(TAG, "Failed to request forced measurement");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }

    // set IIR level for temperature and pressure
    this->iir_config_.bit.set_iir_t = this->iir_temperature_level_;
    this->iir_config_.bit.set_iir_p = this->iir_pressure_level_;

    // write IIR configuration register
    if (!this->write_byte(BMP581_DSP_IIR, this->iir_config_.reg)) {
      ESP_LOGE(TAG, "Failed to write IIR configuration register");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }

    // set data registers to store IIR filtered values if filter is enabled
    this->dsp_config_.bit.shdw_sel_iir_t = (this->iir_temperature_level_ != IIR_FILTER_OFF);
    this->dsp_config_.bit.shdw_sel_iir_p = (this->iir_pressure_level_ != IIR_FILTER_OFF);

    this->dsp_config_.bit.comp_pt_en = 0x3;  // enable pressure temperature compensation (page 61 of datasheet)

    // write data register's IIR source register
    if (!this->write_byte(BMP581_DSP, this->dsp_config_.reg)) {
      ESP_LOGE(TAG, "Failed to write data register's IIR source register");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }
  }

  ////////////////////////////////////
  // 7) Enable data ready interrupt //
  ////////////////////////////////////

  this->int_source_.bit.drdy_data_reg_en = true;  // enable data ready interrupt

  // write interrupt source register
  if (!this->write_byte(BMP581_INT_SOURCE, this->int_source_.reg)) {
    ESP_LOGE(TAG, "Failed to write interrupt enable register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }
}

void BMP581Component::update() {
  /*
   * Each update goes through several stages
   *  0) Verify a temperature or pressure sensor is defined befor proceeding
   *  1) Set forced power mode to request sensor readings
   *  2) Verify sensor has data ready
   *  3) Read data registers for temperature and pressure if applicable
   *  4) Compute and publish temperature measurement if sensor is defined
   *  5) Compute and publish pressure measurement if sensor is defined
   */

  /////////////////////////////////////////////////////////////////////////////
  // 0) Verify a temperature or pressure sensor is defined before proceeding //
  /////////////////////////////////////////////////////////////////////////////

  if ((this->temperature_sensor_ == nullptr) && (this->pressure_sensor_ == nullptr)) {
    return;
  }

  /////////////////////////////////////////////////////////////
  // 1) Set forced power mode to request sensor measurements //
  /////////////////////////////////////////////////////////////

  if (!this->set_power_mode_(FORCED_MODE)) {
    ESP_LOGD(TAG, "Failed to request forced measurement");
    return;
  }

  /////////////////////////////////////
  // 2) Verify sensor has data ready //
  /////////////////////////////////////

  if (!this->check_data_readiness_()) {
    ESP_LOGD(TAG, "Data isn't ready, skipping update");
    return;
  }

  ///////////////////////////////////////////////////////////////////////
  // 3) Read data registers for temperature and pressure if applicable //
  ///////////////////////////////////////////////////////////////////////
  uint8_t data[6];
  uint8_t bytes_to_read = 3;  // temperature only measurement only needs 3 bytes

  if (this->pressure_sensor_) {
    bytes_to_read = 6;  // pressure measurement requires 6 bytes
  }

  if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], bytes_to_read)) {
    ESP_LOGE(TAG, "Failed to read sensor measurement data");
    return;
  }

  //////////////////////////////////////////////////////////////////////////
  // 4) Compute and publish temmperature measurement if sensor is defined //
  //////////////////////////////////////////////////////////////////////////

  if (this->temperature_sensor_) {
    // temperature MSB is in data[2], LSB is in data[1], XLSB in data[0]
    int32_t raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
    float temperature = (float) (raw_temp / 65536.0);

    this->temperature_sensor_->publish_state(temperature);
  }

  //////////////////////////////////////////////////////////////////////
  // 5) Compute and publish pressure measurement if sensor is defined //
  //////////////////////////////////////////////////////////////////////

  if (this->pressure_sensor_) {
    // pressure MSB is in data[5], LSB is in data[4], XLSB is in data[3]
    int32_t raw_press = (int32_t) data[5] << 16 | (int32_t) data[4] << 8 | (int32_t) data[3];
    float pressure = (float) ((raw_press / 64.0) / 100.0);  // Divide by 2^6=64 for Pa, divide by 100 to get hPA

    this->pressure_sensor_->publish_state(pressure);
  }
}

// Check if the BMP581 has measurement data ready to be read
//   - verifies sensor is not in standby mode internally
//   - reads interrupt status register
//   - checks if data ready bit is asserted
//      - internally sets component to standby mode if in forced mode
//   - returns data readiness state
bool BMP581Component::check_data_readiness_() {
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
    // if in forced mode, the set internal record of the power mode to standby
    if (this->odr_config_.bit.pwr_mode == FORCED_MODE) {
      this->odr_config_.bit.pwr_mode = STANDBY_MODE;
    }

    return true;
  }

  return false;
}

// Writes the power mode to the BMP581
//   - updates internal power mode
//   - write odr register on BMP
//   - returns success or failure of write
bool BMP581Component::set_power_mode_(OperationMode mode) {
  this->odr_config_.bit.pwr_mode = mode;

  // write odr register
  return this->write_byte(BMP581_ODR, this->odr_config_.reg);
}

}  // namespace bmp581
}  // namespace esphome
