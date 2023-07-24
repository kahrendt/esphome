#include "qwiic_pir.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace qwiic_pir {

static const char *const TAG = "qwiic_pir";

void QwiicPIRComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Qwiic PIR...");

  // Verify I2C communcation by reading and verifying the chip ID
  uint8_t chip_id;

  if (!this->read_byte(QWIIC_PIR_CHIP_ID, &chip_id)) {
    ESP_LOGE(TAG, "Failed to read the chip's ID");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  if (chip_id != QWIIC_PIR_DEVICE_ID) {
    ESP_LOGE(TAG, "Unknown chip ID, is this a Qwiic PIR?");

    this->error_code_ = ERROR_WRONG_CHIP_ID;
    this->mark_failed();

    return;
  }

  // Configure debounce time
  uint16_t debounce_time = this->debounce_time_;

  if (this->mode_ == HYBRID_MODE)
    debounce_time = 1;

  if (!this->write_byte_16(QWIIC_PIR_DEBOUNCE_TIME, debounce_time)) {
    ESP_LOGE(TAG, "Failed to configure debounce time.");

    this->error_code_ = ERROR_COMMUNICATION_FAILED;
    this->mark_failed();

    return;
  }

  // Publish initial state of sensor
  this->publish_initial_state(false);
}

void QwiicPIRComponent::loop() {
  // Read Event Register
  if (!this->read_byte(QWIIC_PIR_EVENT_STATUS, &this->event_status_.reg)) {
    ESP_LOGW(TAG, "Failed to communicate with sensor");

    return;
  }

  switch (this->mode_) {
    case RAW_MODE:
      this->publish_state(this->event_status_.raw_reading);
      return;
    case DEBOUNCED_MODE:
      // Handle debounced motion events
      if (this->event_status_.event_available) {
        // If an object is detected, publish true
        if (this->event_status_.object_detected)
          this->publish_state(true);

        // If an object has been removed, publish false
        if (this->event_status_.object_removed)
          this->publish_state(false);

        if (!this->write_byte(QWIIC_PIR_EVENT_STATUS, 0x00)) {
          ESP_LOGW(TAG, "Failed to clear events on sensor");
        }
      }
      return;
    case HYBRID_MODE:
      if (this->state) {
        // Sensor state is detecting motion
        if (!this->event_status_.raw_reading) {
          // Raw PIR Sensor is off
          if (millis() - last_on_time_ > this->debounce_time_) {
            // Verify the raw PIR sensor has been off sufficiently long
            this->publish_state(false);
          }
        } else {
          // Raw PIR sensor is on, update last_on_time_
          this->last_on_time_ = millis();
        }
      }

      // Sensor reports an event
      if (this->event_status_.event_available) {
        // Clear event register on sensor
        if (!this->write_byte(QWIIC_PIR_EVENT_STATUS, 0x00)) {
          ESP_LOGW(TAG, "Failed to clear events on sensor");
        }

        if (!this->state) {
          this->publish_state(true);
          this->last_on_time_ = millis();
        }
      }
      return;
  }
}

void QwiicPIRComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Qwiic PIR:");

  ESP_LOGCONFIG(TAG, "  Debounce Time: %ums", this->debounce_time_);

  switch (this->error_code_) {
    case NONE:
      break;
    case ERROR_COMMUNICATION_FAILED:
      ESP_LOGE(TAG, "  Communication with Qwiic PIR failed!");
      break;
    case ERROR_WRONG_CHIP_ID:
      ESP_LOGE(TAG, "  Qwiic PIR has wrong chip ID - please verify you are using a Qwiic PIR");
      break;
    default:
      ESP_LOGE(TAG, "  Qwiic PIR error code %d", (int) this->error_code_);
      break;
  }

  LOG_I2C_DEVICE(this);
  LOG_BINARY_SENSOR("  ", "Qwiic PIR Binary Sensor", this);
}

}  // namespace qwiic_pir
}  // namespace esphome
