/*
 * Adds support for Bosch's BMP581 high accuracy pressure and temperature sensor
 *  - Component structure based on ESPHome's BMP3XX component (as of March, 2023)
 *    - Implementation is easier as the sensor itself automatically compensates pressure for the temperature
 *      - Temperature and pressure data is converted via simple divison operations in this component
 *    - IIR filter level can independently be applied to temperature and pressure measurements
 *  - Bosch's BMP5-Sensor-API was consulted to verify that sensor configuration is done correctly
 *    - Copyright (c) 2022 Bosch Sensortec Gmbh, SPDX-License-Identifier: BSD-3-Clause
 *  - This component uses forced power mode only so measurements are synchronized by the host
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
    case ERROR_PRIME_IIR_FAILED:
      ESP_LOGE(TAG, " BMP581's IIR Filter failed to prime with an initial measurement");
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
    ESP_LOGCONFIG(TAG, "    Over-sampling: %s", LOG_STR_ARG(oversampling_to_str(this->temperature_oversampling_)));
  }

  if (this->pressure_sensor_) {
    LOG_SENSOR("  ", "Pressure", this->pressure_sensor_);
    ESP_LOGCONFIG(TAG, "    IIR Filter: %s", LOG_STR_ARG(iir_filter_to_str(this->iir_pressure_level_)));
    ESP_LOGCONFIG(TAG, "    Over-sampling: %s", LOG_STR_ARG(oversampling_to_str(this->pressure_oversampling_)));
  }
}

void BMP581Component::setup() {
  /*
   * Setup goes through several stages, which follows the post-power-up procedure (page 18 of datasheet) and then sets
   * configured options
   *  1) Soft reboot
   *  2) Verify ASIC chip ID matches BMP581
   *  3) Verify sensor status (check if NVM is okay)
   *  4) Enable data ready interrupt
   *  5) Write initial configuration values and write IIR settings, if configured
   *  6) Write the configured over-sampling rates for all future measurements
   */

  this->error_code_ = NONE;
  ESP_LOGCONFIG(TAG, "Setting up BMP581...");

  ////////////////////
  // 1) Soft reboot //
  ////////////////////

  // Power-On-Reboot bit is asserted if sensor successfully reset
  if (!this->reset_()) {
    ESP_LOGE(TAG, "BMP581 failed to reset");

    this->error_code_ = ERROR_SENSOR_RESET;
    this->mark_failed();

    return;
  }

  ///////////////////////////////////////////
  // 2) Verify ASIC chip ID matches BMP581 //
  ///////////////////////////////////////////

  uint8_t chip_id;

  // read chip id from sensor
  if (!this->read_byte(BMP581_CHIP_ID, &chip_id)) {
    ESP_LOGE(TAG, "Failed to read chip id");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  // verify id
  if (chip_id != BMP581_ASIC_ID) {
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

  // verify status_nvm_rdy bit (it is asserted if boot was successful)
  if (!(this->status_.bit.status_nvm_rdy)) {
    ESP_LOGE(TAG, "NVM not ready after boot");

    this->error_code_ = ERROR_SENSOR_STATUS;
    this->mark_failed();

    return;
  }

  // verify status_nvm_err bit (it is asserted if an error is detected)
  if (this->status_.bit.status_nvm_err) {
    ESP_LOGE(TAG, "NVM error detected on boot");

    this->error_code_ = ERROR_SENSOR_STATUS;
    this->mark_failed();

    return;
  }

  ////////////////////////////////////
  // 4) Enable data ready interrupt //
  ////////////////////////////////////

  this->int_source_.bit.drdy_data_reg_en = true;  // enable data ready interrupt

  // write interrupt source register
  if (!this->write_byte(BMP581_INT_SOURCE, this->int_source_.reg)) {
    ESP_LOGE(TAG, "Failed to write interrupt source register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  /////////////////////////////////////////////////////////////////////////////////
  // 5) Set initial configuration values internally //
  /////////////////////////////////////////////////////////////////////////////////

  // set output data rate to 4 Hz=0x19 (page 65 of datasheet)
  //  - ?shouldn't matter? as this component only uses FORCED_MODE - datasheet is ambiguous
  //  - If in NORMAL_MODE or NONSTOP_MODE, then this would still allow deep standby to save power
  //  - will be written to BMP581 at next reading
  this->odr_config_.bit.odr = 0x19;

  // configure pressure readings, if sensor is defined
  if (this->pressure_sensor_ != nullptr) {
    this->osr_config_.bit.press_en = true;
  }

  /////////////////////////////////////////////////////////////////////////////
  // 6) Write the configured over-sampling rates for all future measurements //
  /////////////////////////////////////////////////////////////////////////////

  // set the measurement timeout for readings based on the currenet over-sampling rates
  this->measurement_time_ =
      this->determine_conversion_time_(this->temperature_oversampling_, this->pressure_oversampling_);

  // write settings to over-sampling register
  if (!this->write_oversampling_(this->temperature_oversampling_, this->pressure_oversampling_)) {
    // if (!this->write_byte(BMP581_OSR, this->osr_config_.reg)) {
    ESP_LOGE(TAG, "Failed to write over-sampling register");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  ////////////////////////////////////////////////////
  /// 7) Enable and prime IIR Filter(s), if enabled //
  ////////////////////////////////////////////////////

  if ((this->iir_temperature_level_ != IIR_FILTER_OFF) || (this->iir_pressure_level_ != IIR_FILTER_OFF)) {
    if (!this->write_iir_(this->iir_temperature_level_, this->iir_pressure_level_)) {
      ESP_LOGE(TAG, "Failed to write IIR configuration registers");

      this->error_code_ = ERROR_COMMUNICATION_FAILED;
      this->mark_failed();

      return;
    }

    if (!this->prime_iir_filter_()) {
      ESP_LOGE(TAG, "Failed to prime the IIR filter with an intiial measurement");

      this->error_code_ = ERROR_PRIME_IIR_FAILED;
      this->mark_failed();

      return;
    }
  }
}

void BMP581Component::update() {
  /*
   * Each update goes through several stages
   *  0) Verify either a temperature or pressure sensor is defined before proceeding
   *  1) Start a measurement
   *  2) Wait for measurement to finish (based on over-sampling rates)
   *  3) Read data registers for temperature and pressure, if applicable
   *  4) Publish measurements to sensor, if applicable
   */

  ////////////////////////////////////////////////////////////////////////////////////
  // 0) Verify either a temperature or pressure sensor is defined before proceeding //
  ////////////////////////////////////////////////////////////////////////////////////

  if ((this->temperature_sensor_ == nullptr) && (this->pressure_sensor_ == nullptr)) {
    return;
  }

  //////////////////////////////////////////////////////////////
  // 1) Set forced power mode to initiate sensor measurements //
  //////////////////////////////////////////////////////////////

  if (!this->start_measurement_()) {
    ESP_LOGW(TAG, "Failed to request forced measurement of sensors");
    this->status_set_warning();

    return;
  }

  //////////////////////////////////////////////////////////////////////
  // 2) Wait for measurement to finish (based on over-sampling rates) //
  //////////////////////////////////////////////////////////////////////

  this->set_timeout("measurement", this->measurement_time_, [this]() {
    float temperature = 0.0;
    float pressure = 0.0;

    ////////////////////////////////////////////////////////////////////////
    // 3) Read data registers for temperature and pressure, if applicable //
    ////////////////////////////////////////////////////////////////////////

    if (this->pressure_sensor_ != nullptr) {
      if (!this->read_temperature_and_pressure_(temperature, pressure)) {
        ESP_LOGW(TAG, "Failed to read temperature and pressure measurements, skipping update");
        this->status_set_warning();

        return;
      }
    } else {
      if (!this->read_temperature_(temperature)) {
        ESP_LOGW(TAG, "Failed to read temperature measurement, skipping update");
        this->status_set_warning();

        return;
      }
    }

    //////////////////////////////////////////////////////
    // 4) Publish measurements to sensor, if applicable //
    //////////////////////////////////////////////////////

    if (this->temperature_sensor_ != nullptr) {
      this->temperature_sensor_->publish_state(temperature);
    }

    if (this->pressure_sensor_ != nullptr) {
      this->pressure_sensor_->publish_state(pressure);
    }

    this->status_clear_warning();
  });
}

// Checks if the BMP581 has measurement data ready
//   - verifies component is not internally in standby mode
//   - reads interrupt status register
//   - checks if data ready bit is asserted
//      - If true, then internally sets component to standby mode if in forced mode
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
    // If in forced mode, then set internal record of the power mode to standby
    //  - sensor automatically returns to standby mode after completing a forced measurement
    if (this->odr_config_.bit.pwr_mode == FORCED_MODE) {
      this->odr_config_.bit.pwr_mode = STANDBY_MODE;
    }

    return true;
  }

  return false;
}

// Prime the IIR filter with an initial reading
//  - disables oversampling for a fast initial measurement; avoids slowing down ESPHome's startup process
//  - enable IIR filter flushing with forced measurements
//  - force a measurement; flushing the IIR filter and priming it with a current value
//  - disable IIR filter flushing with forced measurements
//  - returns success of priming
bool BMP581Component::prime_iir_filter_() {
  // disable over-sampling for temperature and pressure for a fast initial measurement
  if (!this->write_oversampling_(OVERSAMPLING_NONE, OVERSAMPLING_NONE)) {
    ESP_LOGE(TAG, "Failed to write over-sampling register");

    return false;
  }

  // flush the IIR filter with forced measurements (If IIR filter is configured, then we will only flush once)
  this->dsp_config_.bit.iir_flush_forced_en = true;

  // write data register's IIR source register
  if (!this->write_byte(BMP581_DSP, this->dsp_config_.reg)) {
    ESP_LOGE(TAG, "Failed to write IIR source register");

    return false;
  }

  // forces an intial measurement by writing to the output data rate register
  //  - this measurements flushes the IIR filter reflecting the DSP settings
  //  - flushing with this initial reading avoids having the internal previous data aquisition being 0, which
  //    (I)nfinitely affects future values
  if (!this->start_measurement_()) {
    ESP_LOGE(TAG, "Failed to request a forced measurement");

    return false;
  }

  // with over-sampling disabled, the conversion time for one measurement is ceilf(1.5*(1.0+1.0)) = 3ms
  delay(3);

  if (!this->check_data_readiness_()) {
    ESP_LOGE(TAG, "IIR priming measurement was not ready");

    return false;
  }

  this->dsp_config_.bit.iir_flush_forced_en = false;

  return this->write_byte(BMP581_DSP, this->dsp_config_.reg);
}

// Determines the conversion time needed for a measurement based on over-sampling settings
//  - returns a rounded up time in ms
uint16_t BMP581Component::determine_conversion_time_(Oversampling temperature_oversampling,
                                                     Oversampling pressure_oversampling) {
  float temperature_conversion_times[8] = {1.0, 1.1, 1.5, 2.1, 3.3, 5.8, 10.8, 20.8};  // page 12 of datasheet
  float pressure_conversion_times[8] = {1.0, 1.7, 2.9, 5.4, 10.4, 20.4, 40.4, 80.4};   // page 12 of datasheet

  // Datasheet indicates a 5% possible error in each conversion
  float total_time = 1.05 * (temperature_conversion_times[temperature_oversampling] +
                             pressure_conversion_times[pressure_oversampling]);

  return (uint16_t) ceilf(total_time);
}

// Reads a temperature measurement from sensor
//  - verifies data is ready to be read
//  - reads in 3 bytes of temperature data
//  - returns whether successful, where the passed in variabe stores the measured temperature (in degrees Celsius)
bool BMP581Component::read_temperature_(float &temperature) {
  if (!this->check_data_readiness_()) {
    ESP_LOGW(TAG, "Data from sensor isn't ready, skipping this update");
    this->status_set_warning();

    return false;
  }

  uint8_t data[3];
  if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], 3)) {
    ESP_LOGW(TAG, "Failed to read sensor's measurement data");
    this->status_set_warning();

    return false;
  }

  // temperature MSB is in data[2], LSB is in data[1], XLSB in data[0]
  int32_t raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
  temperature = (float) (raw_temp / 65536.0);  // convert measurement to degrees Celsius (page 22 of datasheet)

  return true;
}

// Reads a pressure measurement from sensor
//  - temperature measurement is always enabled on sensor, so read both temperature and pressure
//  - returns whether successful, where the passed in variable stores the measured pressure (in hPa)
bool BMP581Component::read_pressure_(float &pressure) {
  float temperature;
  return this->read_temperature_and_pressure_(temperature, pressure);
}

// Reads a temperature and a pressure measurement from sensor
//  - verifies data is ready to be read
//  - reads in 6 bytes of temperature data (3 for temeperature, 3 for pressure)
//  - returns whether successful, where the passed in variables store
//    - the measured temperature (in degrees Celsius)
//    - the measured pressure (in hPa)
bool BMP581Component::read_temperature_and_pressure_(float &temperature, float &pressure) {
  if (!this->check_data_readiness_()) {
    ESP_LOGW(TAG, "Data from sensor isn't ready, skipping this update");
    this->status_set_warning();

    return false;
  }

  uint8_t data[6];
  if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], 6)) {
    ESP_LOGW(TAG, "Failed to read sensor's measurement data");
    this->status_set_warning();

    return false;
  }

  // temperature MSB is in data[2], LSB is in data[1], XLSB in data[0]
  int32_t raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
  temperature = (float) (raw_temp / 65536.0);  // convert measurement to degrees Celsius (page 22 of datasheet)

  // pressure MSB is in data[5], LSB is in data[4], XLSB in data[3]
  int32_t raw_press = (int32_t) data[5] << 16 | (int32_t) data[4] << 8 | (int32_t) data[3];
  pressure = (float) ((raw_press / 64.0) /
                      100.0);  // Divide by 2^6=64 for Pa (page 22 of datasheet), divide by 100 to convert to hPa

  return true;
}

// Soft reset the BMP581
//  - writes reset command to command
//  - waits for sensor to complete reset
//  - returns the Power-On-Reboot interrupt status, which is asserted if successful
bool BMP581Component::reset_() {
  // writes reset command to BMP's command register
  if (!this->write_byte(BMP581_COMMAND, RESET_COMMAND)) {
    ESP_LOGE(TAG, "Failed to write reset command");

    return false;
  }

  // t_{soft_res} = 2ms (page 11 of datasheet); time it takes to enter standby mode
  //  - round up to 3 ms
  delay(3);

  // read interrupt status register
  if (!this->read_byte(BMP581_INT_STATUS, &this->int_status_.reg)) {
    ESP_LOGE(TAG, "Failed to read interrupt status register");

    return false;
  }

  // Power-On-Reboot bit is asserted if sensor successfully reset
  return this->int_status_.bit.por;
}

// Starts a measurement on sensor
//  - only pushes the sensor into FORCED_MODE for a reading if already in STANDBY_MODE
//  - returns whether a measurement is in progress
bool BMP581Component::start_measurement_() {
  if (this->odr_config_.bit.pwr_mode == STANDBY_MODE) {
    return this->write_power_mode_(FORCED_MODE);
  } else {
    return true;
  }
}

// Writes the IIR filter configuration to sensor
//  - ensures data registers store filtered values
//  - sets IIR filter levels to sensor
//  - match other default settings on sensor
//  - writes configuration to the two relevant registers
//  - returns success or failure of write to the registers
bool BMP581Component::write_iir_(IIRFilter temperature_iir, IIRFilter pressure_iir) {
  // If the temperature/pressure IIR filter is configured, then ensure data registers store the filtered measurement
  this->dsp_config_.bit.shdw_sel_iir_t = (temperature_iir != IIR_FILTER_OFF);
  this->dsp_config_.bit.shdw_sel_iir_p = (pressure_iir != IIR_FILTER_OFF);

  // set temperature and pressure IIR filter level to configured values
  this->iir_config_.bit.set_iir_t = temperature_iir;
  this->iir_config_.bit.set_iir_p = pressure_iir;

  // enable pressure and temperature compensation (page 61 of datasheet)
  //  - ?only relevant if IIR filter is applied?; the datasheet is ambiguous
  //  - matches BMP's default setting
  this->dsp_config_.bit.comp_pt_en = 0x3;

  // BMP581_DSP register and BMP581_DSP_IIR registers are successive
  //  - write the configuration IIR configuration with one command
  uint8_t register_data[2] = {dsp_config_.reg, iir_config_.reg};
  return this->write_bytes(BMP581_DSP, register_data, 2);
}

// Set the over-sampling settings on the BMP581
//  - updates component's internal setting
//  - returns success or failure of write to Over-Sampling Rate register
bool BMP581Component::write_oversampling_(Oversampling temperature_oversampling, Oversampling pressure_oversampling) {
  this->osr_config_.bit.osr_t = temperature_oversampling;
  this->osr_config_.bit.osr_p = pressure_oversampling;

  return this->write_byte(BMP581_OSR, osr_config_.reg);
}

// Set the power mode on the BMP581
//   - updates the component's internal power mode
//   - writes odr register on BMP
//   - returns success or failure of write to Output Data Rate register
bool BMP581Component::write_power_mode_(OperationMode mode) {
  this->odr_config_.bit.pwr_mode = mode;

  // write odr register
  return this->write_byte(BMP581_ODR, this->odr_config_.reg);
}

}  // namespace bmp581
}  // namespace esphome
