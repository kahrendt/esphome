#include "bmp581.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace bmp581 {

static const char *const TAG = "bmp581";

void BMP581Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMP581:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
}

void BMP581Component::setup() {
  // page 18 of data sheet has a post-power up procedure
  // read chip_id
  // read status register (0x28) and check status_nvm_rdy==1, status_nvm_err == 0
  // read int_status.por (0x27) register and check it is 1

  if (!this->reset_()) {
    this->mark_failed();
    return;
  }

  if (!this->verify_chip_id_()) {
    this->mark_failed();
    return;
  }

  // read and verify device status
  if (!this->verify_status_()) {
    this->mark_failed();
    return;
  }

  // set output data rate and power mode
  this->odr_config_.bit.pwr_mode = STANDBY_MODE;  // standby mode to start
  this->odr_config_.bit.odr =
      0x19;  // 4 Hz data out rate; shouldn't matter as we are in forced mode (though allows deep standby?)

  if (!this->set_odr_register_(odr_config_.reg)) {
    this->mark_failed();
    return;
  }
  delay(10);

  // set up oversampling
  // Disable pressure readings/oversampling if no sensor is defined
  if (this->pressure_sensor_ == nullptr) {
    this->osr_config_.bit.osr_p = OVERSAMPLING_NONE;
    this->osr_config_.bit.press_en = false;
  } else {
    this->osr_config_.bit.osr_p = this->pressure_oversampling_;
    this->osr_config_.bit.press_en = true;
  }

  // Disable temperature oversampling if no sensor is defined (sensor still internally compensates)
  if (this->temperature_sensor_ == nullptr) {
    this->osr_config_.bit.osr_t = OVERSAMPLING_NONE;
  } else {
    this->osr_config_.bit.osr_t = this->temperature_oversampling_;
  }

  // set oversampling register
  if (!this->set_osr_register_(osr_config_.reg)) {
    this->mark_failed();
    return;
  }
  delay(10);

  // If an IIR filter is enabled, set it all up
  if ((this->iir_temperature_level_ != IIR_FILTER_OFF) || (this->iir_pressure_level_ != IIR_FILTER_OFF)) {
    // read in one data point to prime the IIR filter
    this->set_mode_(FORCED_MODE);

    // set IIR level for temperature and pressure
    this->iir_config_.bit.set_iir_t = this->iir_temperature_level_;
    this->iir_config_.bit.set_iir_p = this->iir_pressure_level_;
    if (!this->write_byte(BMP581_DSP_IIR, this->iir_config_.reg)) {
      ESP_LOGE(TAG, "Failed to set IIR register");
      this->mark_failed();
      return;
    }
    delay(10);

    // set data registers to store IIR filtered values
    this->dsp_config_.bit.shdw_sel_iir_t = (this->iir_temperature_level_ != IIR_FILTER_OFF);
    this->dsp_config_.bit.shdw_sel_iir_p = (this->iir_pressure_level_ != IIR_FILTER_OFF);

    this->dsp_config_.bit.comp_pt_en = 0x3;  // enable pressure temperature compensation

    if (!this->write_byte(BMP581_DSP, this->dsp_config_.reg)) {
      ESP_LOGE(TAG, "Failed to set data register's IIR source");
      this->mark_failed();
      return;
    }
    delay(10);
  }

  // set up interrupts
  int_source_.bit.drdy_data_reg_en = 0x1;  // enable data ready interrupt

  if (!this->set_interrupt_source_register_(int_source_.reg)) {
    this->mark_failed();
    return;
  }
  delay(10);
}

void BMP581Component::update() {
  uint8_t data[6];

  if (!set_mode_(FORCED_MODE)) {
    ESP_LOGD("bmpt81.sensor", "Forced reading request failed");
    return;
  }

  if (!this->get_data_ready_status_()) {
    ESP_LOGD(TAG, "Data isn't ready, skipping update.");
    return;
  }

  if (this->pressure_sensor_ == nullptr) {  // only read temperature data if no pressure sensor is defined
    if (!this->read_bytes(
            BMP581_MEASUREMENT_DATA, &data[0],
            3)) {  // read 3 bytes of data starting at register 0x1D (temperature xlsb) through 0x1F (temperature MSB)
      ESP_LOGE(TAG, "Failed to read sensor data");
      return;
    }
  } else {  // pressure sensor is defined
    if (!this->read_bytes(
            BMP581_MEASUREMENT_DATA, &data[0],
            6)) {  // read 6 bytes of data starting at register 0x1D (temperature xlsb) through 0x22 (pressure MSB)
      ESP_LOGE(TAG, "Failed to read sensor data");
      return;
    }

    int32_t raw_press = (int32_t) data[5] << 16 | (int32_t) data[4] << 8 | (int32_t) data[3];
    float pressure = (float) ((raw_press / 64.0) / 100.0);  // Divide by 2^6=64 for Pa, divide by 100 to get hPA

    this->pressure_sensor_->publish_state(pressure);
  }

  if (this->temperature_sensor_ != nullptr) {  // update temperature sensor only if defined
    int32_t raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
    float temperature = (float) (raw_temp / 65536.0);

    this->temperature_sensor_->publish_state(temperature);
  }
}

// returns if the sensor has data able to be read in
bool BMP581Component::get_data_ready_status_() {
  if (odr_config_.bit.pwr_mode == STANDBY_MODE) {
    ESP_LOGD(TAG, "Data not ready, sensor is in standby mode");
    return false;
  }

  uint8_t status;

  if (!this->read_byte(BMP581_INT_STATUS, &status)) {
    ESP_LOGE(TAG, "Failed to read interrupt status register");
    return false;
  }

  this->int_status_.reg = status;

  if (int_status_.bit.drdy_data_reg) {
    // check if in forced mode, put sensor to sleep if so
    if (odr_config_.bit.pwr_mode == FORCED_MODE) {
      odr_config_.bit.pwr_mode = STANDBY_MODE;
    }

    return true;
  }

  return false;
}

// configure interrupts by writing to interrupt source register and return success
bool BMP581Component::set_interrupt_source_register_(uint8_t reg_value) {
  if (!this->write_byte(BMP581_INT_SOURCE, reg_value)) {
    ESP_LOGE(TAG, "Failed to set interrupt source register");

    return false;
  }

  return true;
}

// configure output data rate and power mode by writing to ODR register and return success
bool BMP581Component::set_odr_register_(uint8_t reg_value) {
  if (!this->write_byte(BMP581_ODR, reg_value)) {
    ESP_LOGE(TAG, "Failed to set ODR register/power mode");

    return false;
  }

  return true;
}

// configure oversampling by writing to OSR register and return success
bool BMP581Component::set_osr_register_(uint8_t reg_value) {
  if (!this->write_byte(BMP581_OSR, reg_value)) {
    ESP_LOGE(TAG, "Failed to set oversampling register");

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
    ESP_LOGE(TAG, "Unknown chip id, is this a BMP581?");
    this->mark_failed();
    return false;
  }

  return true;
}

// get the status and check for readiness & errors and return success
bool BMP581Component::verify_status_() {
  if (!this->read_byte(BMP581_STATUS, &this->status_.reg)) {
    ESP_LOGE(TAG, "Failed to get status register");

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
    ESP_LOGE(TAG, "Failed to send reset command");
    return false;
  }
  delay(5);  // t_{soft_res} is 2ms (page 11 of datasheet))

  if (!this->read_byte(BMP581_INT_STATUS, &this->int_status_.reg)) {
    ESP_LOGE(TAG, "Failed to get interrupt status");
    return false;
  }

  // POR bit returns if reboot was successful or not
  return int_status_.bit.por;
}

}  // namespace bmp581
}  // namespace esphome
