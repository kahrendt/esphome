#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace qwiic_pir {

// Qwiic PIR Register Addresses
enum {
  QWIIC_PIR_CHIP_ID = 0x00,
  QWIIC_PIR_EVENT_STATUS = 0x03,
  QWIIC_PIR_DEBOUNCE_TIME = 0x05,
};

static const uint8_t QWIIC_PIR_DEVICE_ID = 0x72;

class QwiicPIRComponent : public Component, public i2c::I2CDevice, public binary_sensor::BinarySensor {
 public:
  void setup() override;
  void loop() override;

  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_debounce_time(uint16_t debounce_time) { this->debounce_time_ = debounce_time; }
  void set_raw_binary_sensor(binary_sensor::BinarySensor *raw_binary_sensor) {
    this->raw_binary_sensor_ = raw_binary_sensor;
  }

 protected:
  binary_sensor::BinarySensor *raw_binary_sensor_{nullptr};

  uint16_t debounce_time_;

  enum ErrorCode {
    NONE = 0,
    ERROR_COMMUNICATION_FAILED,
    ERROR_WRONG_CHIP_ID,
  } error_code_{NONE};

  union {
    struct {
      bool raw_reading : 1;
      bool event_available : 1;
      bool object_removed : 1;
      bool object_detected : 1;
    } bit;
    uint8_t reg;
  } event_status_ = {.reg = 0};
};

}  // namespace qwiic_pir
}  // namespace esphome
