#pragma once

#include "esphome/components/audio_dac/audio_dac.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"

namespace esphome {
namespace es9018k2m {

// ES9018K2M Register Addresses
static const uint8_t ES9018K2M_REG_GENERAL_SETTINGS = 0x07;  // General Settings (mute control)
static const uint8_t ES9018K2M_REG_VOLUME_1 = 0x0F;          // Volume 1 (Left channel)
static const uint8_t ES9018K2M_REG_VOLUME_2 = 0x10;          // Volume 2 (Right channel)

class ES9018K2M : public audio_dac::AudioDac, public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;

  bool set_mute_off() override;
  bool set_mute_on() override;
  bool set_volume(float volume) override;

  bool is_muted() override;
  float volume() override;

 protected:
  bool write_mute_();
  bool write_volume_();

  float volume_{1.0f};  // Default to full volume
};

}  // namespace es9018k2m
}  // namespace esphome
