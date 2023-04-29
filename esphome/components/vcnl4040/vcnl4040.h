// All datasheet page references refer to Vishay document number 84274 Revision 1.7 dated 04-Nov-2020

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace vcnl4040 {

static const uint16_t VCNL4040_CHIP_ID = 0x86;  // VCNL4040's chip ID Lower Byte (page 11 of datasheet)

// VCNL4040 Register Addresses
enum {
  VCNL4040_ALS_CONF = 0x00,       // ambient light sensor config
  VCNL4040_PS_CONF_FIRST = 0x03,  // proximity sensor config 1 and 2
  VCNL4040_PS_CONF_LAST = 0x04,   // proximity sensor config 3 and mode
  VCNL4040_PS_OUTPUT = 0x08,      // proximity sensor output
  VCNL4040_ALS_OUTPUT = 0x09,     // ambient light sensor output
  VCNL4040_WHITE_OUTPUT = 0x0A,   // white channel sensor output
  VCNL4040_ID = 0x0C,             // Device ID LSB and MSB
};

enum AmbientIntegrationTime {
  ALS_80 = 0x0,
  ALS_160 = 0x1,
  ALS_320 = 0x2,
  ALS_640 = 0x3,
};

enum IREDDuty {
  IRED_DUTY_40 = 0x0,
  IRED_DUTY_80 = 0x1,
  IRED_DUTY_160 = 0x2,
  IRED_DUTY_320 = 0x3,
};

enum ProximityIntegrationTime {
  PS_IT_1T = 0x0,
  PS_IT_1T5 = 0x1,
  PS_IT_2T = 0x2,
  PS_IT_2T5 = 0x3,
  PS_IT_3T = 0x4,
  PS_IT_3T5 = 0x5,
  PS_IT_4T = 0x6,
  PS_IT_8T = 0x7,
};

enum ProximityOutputResolution {
  PS_RESOLUTION_12 = 0x0,
  PS_RESOLUTION_16 = 0x1,
};

class VCNL4040Component : public PollingComponent, public i2c::I2CDevice {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;
  void update() override;

  void set_lux_sensor(sensor::Sensor *lux_sensor) { this->lux_sensor_ = lux_sensor; }
  void set_proximity_sensor(sensor::Sensor *proximity_sensor) { this->proximity_sensor_ = proximity_sensor; }
  void set_white_channel_sensor(sensor::Sensor *white_channel_sensor) {
    this->white_channel_sensor_ = white_channel_sensor;
  }

  void set_als_integration_time_config(AmbientIntegrationTime als_integration_time) {
    this->als_integration_time_ = als_integration_time;
  }
  void set_ired_duty_config(IREDDuty ired_duty) { this->ired_duty_ = ired_duty; }
  void set_proximity_integration_time_config(ProximityIntegrationTime proximity_integration_time) {
    this->proximity_integration_time_ = proximity_integration_time;
  }
  void set_proximity_output_resolution(ProximityOutputResolution proximity_output_resolution) {
    this->proximity_output_resolution_ = proximity_output_resolution;
  }

 protected:
  sensor::Sensor *lux_sensor_{nullptr};
  sensor::Sensor *proximity_sensor_{nullptr};
  sensor::Sensor *white_channel_sensor_{nullptr};

  AmbientIntegrationTime als_integration_time_;
  IREDDuty ired_duty_;
  ProximityIntegrationTime proximity_integration_time_;
  ProximityOutputResolution proximity_output_resolution_;

  uint16_t read_sensor_without_stop_(uint8_t register_address);

  bool read_ambient_light_(float &ambient_light);
  bool read_proximity_(float &proximity);
  bool read_white_channel_(float &white_channel);

  bool write_als_config_settings_();
  bool write_ps_config_settings_();

  enum ErrorCode {
    NONE = 0,
    ERROR_COMMUNICATION_FAILED,
    ERROR_WRONG_CHIP_ID,
  } error_code_{NONE};

  // VCNL4040's ALS_CONF register (command code 0x00) for the Ambient Light Sensor, page 9 of datasheet
  union {
    struct {
      uint8_t als_sd : 1;      // ALS power on (if 0), ALS shutdown if 1
      uint8_t als_int_en : 1;  // ALS interupt enable
      uint8_t als_pers : 2;    // ALS interrupt persistence setting
      uint8_t : 2;             // reserved
      uint8_t als_it : 2;      // ALS integration time setting
    } bit;
    uint8_t reg;
  } als_conf_ = {.reg = 0x01};

  // VCNL4040's PS_CONF1 register (command code 0x03 LSB) for the Proximity Sensor, page 10 of datasheet
  union {
    struct {
      uint8_t ps_sd : 1;    // PS power on (if 0), PS shutdown if 1
      uint8_t ps_it : 3;    // PS integration time
      uint8_t ps_pers : 2;  // PS interrupt persistence setting
      uint8_t ps_duty : 2;  // PS IRED on/off duty ratio
    } bit;
    uint8_t reg;
  } ps_conf1_ = {.reg = 0x01};

  // VCNL4040's PS_CONF2 register (command code 0x03 MSB) for the Proximity Sensor, page 10 of datasheet
  union {
    struct {
      uint8_t ps_int : 2;  // PS interrupt mode
      uint8_t : 1;         // Reserved
      uint8_t ps_hd : 3;   // high resolution setting (12 bits if = 0, 16 if = 1)
    } bit;
    uint8_t reg;
  } ps_conf2_ = {.reg = 0x00};

  // VCNL4040's PS_CONF3 register (command code 0x04 LSB) for the Proximity Sensor, page 10 of datasheet
  union {
    struct {
      uint8_t ps_sc_en : 1;       // PS sunlight cancellation (enabled = 1)
      uint8_t : 1;                // Reserved
      uint8_t ps_trig : 1;        // PS active force mode trigger
      uint8_t ps_af : 1;          // PS active force mode (enabled = 1)
      uint8_t ps_smart_pers : 1;  // PS smart persistence (enabled = 1)
      uint8_t ps_mps : 2;         // PS multi pulse numbers
    } bit;
    uint8_t reg;
  } ps_conf3_ = {.reg = 0x00};

  // VCNL4040's PS_MS register (command code 0x04 MSB) for the Proximity Sensor, page 11 of datasheet
  union {
    struct {
      uint8_t led_i : 3;     // LED current selection
      uint8_t : 3;           // Reserved
      uint8_t ps_ms : 1;     // 0 = proximity normal operation with interrupt function, 1 = proximity detection logic
                             // output mode enable
      uint8_t white_en : 1;  // white channel (enabled = 1)
    } bit;
    uint8_t reg;
  } ps_ms_ = {.reg = 0x00};
};

}  // namespace vcnl4040
}  // namespace esphome
