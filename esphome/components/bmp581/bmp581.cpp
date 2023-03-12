#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "bmp581.h"

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


    if (!this->bmp581_reset_())
    {
    this->mark_failed();
    return;
    }

    if (!this->bmp581_verify_chip_id_()) {
    this->mark_failed();
    return;
    }

    // read and verify device status
    if (!this->bmp581_verify_status_()) {
    this->mark_failed();
    return;
    }

    // set output data rate and power mode
    this->odr_config_.bit.pwr_mode = STANDBY_MODE;  // standby mode to start
    this->odr_config_.bit.odr = 0x1C;       // 1 Hz output data rate
    // this->odr_config_.bit.pwr_mode = NORMAL_MODE;
    // this->odr_config_.bit.odr = 0x18; // 5 hz output data rate
    
    if (!this->bmp581_set_odr_register_(odr_config_.reg)) {
    this->mark_failed();
    return;
    }

    delay(10);


    // set up oversampling
    this->osr_config_.bit.osr_t = this->temperature_oversampling_;
    this->osr_config_.bit.osr_p = this->pressure_oversampling_;
    this->osr_config_.bit.press_en = 0x1;               // enable pressure readings
    
    if (!this->bmp581_set_osr_register_(osr_config_.reg)) {
    this->mark_failed();
    return;
    }
    delay(10);

    // // set iir config, dsp register is 0x30, 0x31 is dsp iir register (default is no iir filter)
    // iir_config_.bit.set_iir_t = IIR_FILTER_128;
    // iir_config_.bit.set_iir_p = IIR_FILTER_128;
    // this->write_byte(BMP581_DSP_IIR, this->iir_config_.reg);

    // delay(10);

    // this->write_byte(0x30, 0b101011);
    // delay(10);

    // set interrupts
    int_source_.bit.drdy_data_reg_en = 0x1;   // enable data ready interrupt
    
    if (!this->bmp581_set_interrupt_source_register_(int_source_.reg)) {
    this->mark_failed();
    return;
    }

    delay(10);
}

void BMP581Component::update() {
    uint8_t data[6];

    int32_t raw_temp = 0;
    int32_t raw_press = 0;

    // if in standby, then switch to forced mode to get a reading
    // if (odr_config_.bit.pwr_mode == STANDBY_MODE) {
    if (!bmp581_set_mode_(FORCED_MODE)) {
        ESP_LOGD("bmpt81.sensor", "Forced reading request failed");
        return;
    }
    delay(100);
    if (!this->bmp581_get_data_ready_status_()) {
        ESP_LOGD(TAG, "Data isn't ready, skipping update.");
        return;
    }
    // }
    // otherwise we are in normal or continuosu mode, so just read
    if (!this->read_bytes(BMP581_MEASUREMENT_DATA, &data[0], 6)){    // read 6 bytes of data starting at register 0x1D (temperature xlsb) through 0x22 (pressure MSB)
    ESP_LOGE(TAG, "Failed to read sensor data");
    return;
    }

    raw_temp = (int32_t) data[2] << 16 | (int32_t) data[1] << 8 | (int32_t) data[0];
    raw_press = (int32_t) data[5] << 16 | (int32_t) data[4] << 8 | (int32_t) data[3];

    float temperature = (float) (raw_temp/65536.0);
    float pressure = (float) ((raw_press/64.0)/100.0);

    this->temperature_sensor_->publish_state(temperature);
    this->pressure_sensor_->publish_state(pressure);
}

bool BMP581Component::bmp581_verify_chip_id_() {
    uint8_t chip_id;
    if (!this->read_byte(BMP581_CHIP_ID, &chip_id))   // read chip id
    { 
    ESP_LOGE(TAG, "Failed to read chip id");
    return false;
    }

    if (chip_id != BMP581_ID)
    {
    ESP_LOGE(TAG, "Unknown chip id, is this a BMP581?");
    this->mark_failed();
    return false;
    }

    return true;
}


bool BMP581Component::bmp581_verify_status_() {
    if (!this->read_byte(BMP581_STATUS, &this->status_.reg))
    {
      ESP_LOGE(TAG, "Failed to get status register");
      
      return false;
    }

    if (!(this->status_.bit.status_nvm_rdy))    // check status_nvm_rdy bit (should be 1 if okay)
    {
      ESP_LOGE(TAG, "NVM not ready");
      return false;     
    }

    if (this->status_.bit.status_nvm_err)   // check status_nvm_err bit (should be 0 if okay)
    {
      ESP_LOGE(TAG, "NVM error detected");
      return false;     
    }

    return true;
}

bool BMP581Component::bmp581_set_mode_(OperationMode mode) {
    this->odr_config_.bit.pwr_mode = mode;
    return this->bmp581_set_odr_register_(this->odr_config_.reg);
}

bool BMP581Component::bmp581_set_interrupt_source_register_(uint8_t reg_value) {
    if (!this->write_byte(BMP581_INT_SOURCE, reg_value)) {
        ESP_LOGE(TAG, "Failed to set interrupt source register");

        return false;
    }

    return true;
}

bool BMP581Component::bmp581_set_odr_register_(uint8_t reg_value) {
    if (!this->write_byte(BMP581_ODR, reg_value)) {
        ESP_LOGE(TAG, "Failed to set ODR register/power mode");

        return false;
    }

    return true;
}

bool BMP581Component::bmp581_set_osr_register_(uint8_t reg_value) {
    if (!this->write_byte(BMP581_OSR, reg_value)) {
        ESP_LOGE(TAG, "Failed to set oversampling register");

        return false;
    }

    return true;
}

bool BMP581Component::bmp581_reset_() {
    if (!this->write_byte(BMP581_COMMAND, RESET_COMMAND))
    {
        ESP_LOGE(TAG, "Failed to send reset command");
        return false;
    } 
    delay(5);                       // t_{soft_res} is 2ms (page 11 of datasheet))

    if (!this->read_byte(BMP581_INT_STATUS, &this->int_status_.reg)){
        ESP_LOGE(TAG, "Failed to get interrupt status");
        return false;
    }

    // POR bit returns if reboot was successful or not
    return int_status_.bit.por;
}

bool BMP581Component::bmp581_get_data_ready_status_() {
    if (odr_config_.bit.pwr_mode == STANDBY_MODE){
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

}
}