#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include "esphome/components/sendspin/sendspin_hub.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_SENDSPIN_METADATA
#include <sendspin/metadata_role.h>
#endif

namespace esphome::sendspin_ {

/// @brief What a sendspin text sensor publishes. The metadata types require the metadata
/// role (codegen requests it); the pairing types work without any role.
enum class SendspinTextSensorType {
  TITLE,
  ARTIST,
  ALBUM,
  ALBUM_ARTIST,
  PAIRING_TOKEN,
  PAIRING_PIN,
};

class SendspinTextSensor final : public SendspinChild, public text_sensor::TextSensor {
 public:
  void dump_config() override;
  void setup() override;

  void set_sensor_type(SendspinTextSensorType sensor_type) { this->sensor_type_ = sensor_type; }

 protected:
#ifdef USE_SENDSPIN_METADATA
  const char *extract_value_(const sendspin::ServerMetadataStateObject &metadata) const;
#endif
  void publish_if_changed_(const char *value);

  SendspinTextSensorType sensor_type_;
};

}  // namespace esphome::sendspin_
#endif
