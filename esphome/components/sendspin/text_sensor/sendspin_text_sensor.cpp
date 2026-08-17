#include "sendspin_text_sensor.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include <string>

namespace esphome::sendspin_ {

static const char *const TAG = "sendspin.text_sensor";

void SendspinTextSensor::dump_config() { LOG_TEXT_SENSOR("", "Sendspin", this); }

#ifdef USE_SENDSPIN_METADATA
// A field is nullopt when the server has not provided it or has explicitly cleared it. Both mean there is nothing to
// show, so return the empty string and let the caller publish it; returning early would leave the previous track's
// value on display.
//
// The empty string is not the same as unknown. A text sensor reports unknown through the API's missing_state flag,
// which follows has_state(), and has_state() is only ever set, never cleared. Once a real value has been published,
// an empty state is the closest we can get. The numeric sensors publish NAN, which does read as unknown.
const char *SendspinTextSensor::extract_value_(const sendspin::ServerMetadataStateObject &metadata) const {
  switch (this->sensor_type_) {
    case SendspinTextSensorType::TITLE:
      return metadata.title.has_value() ? metadata.title.value().c_str() : "";
    case SendspinTextSensorType::ARTIST:
      return metadata.artist.has_value() ? metadata.artist.value().c_str() : "";
    case SendspinTextSensorType::ALBUM:
      return metadata.album.has_value() ? metadata.album.value().c_str() : "";
    case SendspinTextSensorType::ALBUM_ARTIST:
      return metadata.album_artist.has_value() ? metadata.album_artist.value().c_str() : "";
    default:
      return "";
  }
}
#endif  // USE_SENDSPIN_METADATA

// THREAD CONTEXT: Main loop. Every registered hub callback also fires on the main loop
// (SendspinHub dispatches from client_->loop()).
void SendspinTextSensor::setup() {
  switch (this->sensor_type_) {
    case SendspinTextSensorType::PAIRING_TOKEN:
      // The "SP:..." operator pairing token. The hub publishes it shortly after setup and
      // again if the stored Pairing PSK changes; empty means no token is available.
      this->parent_->add_pairing_token_callback(
          [this](const std::string &token) { this->publish_if_changed_(token.c_str()); });
      break;
    case SendspinTextSensorType::PAIRING_PIN:
      // The dynamic pairing PIN: set while a pairing attempt wants it shown, cleared after.
      this->parent_->add_on_display_pairing_pin_callback(
          [this](const std::string &pin) { this->publish_if_changed_(pin.c_str()); });
      this->parent_->add_on_clear_pairing_pin_callback([this]() { this->publish_if_changed_(""); });
      break;
    default:
#ifdef USE_SENDSPIN_METADATA
      // Metadata types; codegen guarantees the metadata role is compiled in for them.
      this->parent_->add_metadata_update_callback([this](const sendspin::ServerMetadataStateObject &metadata) {
        this->publish_if_changed_(this->extract_value_(metadata));
      });
#endif
      break;
  }
}

// Dedup to avoid frontend churn; TextSensor::publish_state already dedups the string assign but still notifies.
void SendspinTextSensor::publish_if_changed_(const char *value) {
  // The state starts empty, so a field that is already cleared when the first update arrives is suppressed here: the
  // entity stays unknown rather than being dropped out of it for good by an empty publish. Later clears do publish the
  // empty string and fire on_value with it.
  if (this->get_raw_state() != value) {
    this->publish_state(value);
  }
}

}  // namespace esphome::sendspin_

#endif
