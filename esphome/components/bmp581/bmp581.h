#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome.h"


namespace esphome {
namespace bmp581 {

static const uint8_t BMP581_ID = 0x50;
static const uint8_t RESET_COMMAND = 0xB6;

enum {
  BMP581_CHIP_ID = 0x01,
  BMP581_INT_SOURCE = 0x15,
  BMP581_MEASUREMENT_DATA = 0x1D,
  BMP581_INT_STATUS = 0x27,
  BMP581_STATUS = 0x28,
  BMP581_DSP_IIR = 0x31,
  BMP581_OSR = 0x36,
  BMP581_ODR = 0x37,
  BMP581_COMMAND = 0x7E
};

enum OperationMode { STANDBY_MODE = 0x0, NORMAL_MODE = 0x1, FORCED_MODE = 0x2, NONSTOP_MODE = 0x3 };

enum Oversampling {
  OVERSAMPLING_NONE = 0x0,
  OVERSAMPLING_X2 = 0x1,
  OVERSAMPLING_X4 = 0x2,
  OVERSAMPLING_X8 = 0x3,
  OVERSAMPLING_X16 = 0x4,
  OVERSAMPLING_X32 = 0x5,
  OVERSAMPLING_X64 = 0x6,
  OVERSAMPLING_X128 = 0x7
};

enum IIRFilter {
  IIR_FILTER_OFF = 0x0,
  IIR_FILTER_2 = 0x1,
  IIR_FILTER_4 = 0x2,
  IIR_FILTER_8 = 0x3,
  IIR_FILTER_16 = 0x4,
  IIR_FILTER_32 = 0x5,
  IIR_FILTER_64 = 0x6,
  IIR_FILTER_128 = 0x7
};

class BMP581Component : public PollingComponent, public i2c::I2CDevice, public sensor::Sensor {
 public:
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }
  void set_pressure_sensor(sensor::Sensor *pressure_sensor) { this->pressure_sensor_ = pressure_sensor; }

  void set_temperature_oversampling_config(Oversampling temperature_oversampling) { this->temperature_oversampling_ = temperature_oversampling; }
  void set_pressure_oversampling_config(Oversampling pressure_oversampling) { this->pressure_oversampling_ = pressure_oversampling; }
  void set_iir_filter_config(IIRFilter iir_level) { this->iir_level_ = iir_level; }


  void dump_config() override {};

  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override;

  void update() override;

 protected:
  
  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *pressure_sensor_{nullptr};

  Oversampling temperature_oversampling_{};
  Oversampling pressure_oversampling_{};

  IIRFilter iir_level_{};

  // get the chip id from device and verify it matches the known id
  bool bmp581_verify_chip_id_();

  // get the status and check for readiness & errors and return success
  bool bmp581_verify_status_();

  // set the power mode on sensor by writing to ODR register and return success
  bool bmp581_set_mode_(OperationMode mode);

  // configure interrupts by writing to interrupt source register and return success
  bool bmp581_set_interrupt_source_register_(uint8_t reg_value);

  // configure output data rate and power mode by writing to ODR register and return success
  bool bmp581_set_odr_register_(uint8_t reg_value);

  // configure oversampling by writing to OSR register and return success
  bool bmp581_set_osr_register_(uint8_t reg_value);

  // reset the sensor by writing to the command register and return success
  bool bmp581_reset_();

  // returns if the sensor has data able to be read in
  bool bmp581_get_data_ready_status_();

  // BMP581's interrupt source register (address 0x15) to configure which interrupts are enabled (page 54 of datasheet)
  union {  
    struct {
      uint8_t drdy_data_reg_en : 1;    // Data ready interrupt enable
      uint8_t fifo_full_en : 1;        // FIFO full interrupt enable
      uint8_t fifo_ths_en : 1;         // FIFO threshold/watermark interrupt enable
      uint8_t oor_p_en : 1;            // Pressure data out-of-range interrupt enable
    } bit;
    uint8_t reg;
  } int_source_ = {.reg = 0};

  // BMP581's interrupt status register (address 0x27) to determine ensor's current state (page 58 of datasheet)
  union {  
    struct {
      uint8_t drdy_data_reg : 1;    // Data ready
      uint8_t fifo_full : 1;        // FIFO full
      uint8_t fifo_ths : 1;         // FIFO fhreshold/watermark
      uint8_t oor_p : 1;            // Pressure data out-of-range
      uint8_t por : 1;              // POR or software reset complete
    } bit;
    uint8_t reg;
  } int_status_ = {.reg = 0};

  // BMP581's status register (address 0x28) to determine if sensor has setup correctly (page 58 of datasheet)
  union {  
    struct {
      uint8_t status_core_rdy : 1;
      uint8_t status_nvm_rdy : 1;             // NVM is ready of operations
      uint8_t status_nvm_err : 1;             // NVM error
      uint8_t status_nvm_cmd_err : 1;         // Indiciates boot command error
      uint8_t status_boot_err_corrected : 1;  // Indiciates a boot error ahs been corrected     
      uint8_t : 2;
      uint8_t st_crack_pass : 1;              // Indicates crack check has executed without error
    } bit;
    uint8_t reg;
  } status_ = {.reg = 0};

  // BMP581's iir register (address 0x36) to configure iir filtering(page 62 of datasheet)
  union {  
    struct {
      uint8_t set_iir_t : 3;          // Temperature IIR filter coefficient
      uint8_t set_iir_p : 3;          // Pressure IIR filter coefficient
    } bit;
    uint8_t reg;
  } iir_config_ = {.reg = 0};


  // BMP581's osr register (address 0x36) to configure oversampling (page 64 of datasheet)
  union {  
    struct {
      uint8_t osr_t : 3;          // Temperature oversampling
      uint8_t osr_p : 3;          // Pressure oversampling
      uint8_t press_en : 1;       // Enables pressure measurement 
    } bit;
    uint8_t reg;
  } osr_config_ = {.reg = 0};

  // BMP581's odr register (address 0x37) to configure output data rate and power mode (page 64 of datasheet)
  union {  
    struct {
      uint8_t pwr_mode : 2;       // power mode of sensor
      uint8_t odr : 5;            // output data rate
      uint8_t deep_dis : 1;       // disables deep standby  
    } bit;
    uint8_t reg;
  } odr_config_ = {.reg = 0};
};

}   // namespace bmp581
}   // namespace esphome