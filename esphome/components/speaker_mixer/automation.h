#pragma once

#ifdef USE_ESP32

#include "speaker_mixer.h"

namespace esphome {
namespace speaker_mixer {
template<typename... Ts> class DuckingSetAction : public Action<Ts...>, public Parented<SourceSpeaker> {
  TEMPLATABLE_VALUE(uint8_t, decibel_reduction)
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(Ts... x) override {
    this->parent_->set_ducking_reduction(this->decibel_reduction_.value(x...), this->duration_.value(x...));
  }
};
}  // namespace speaker_mixer
}  // namespace esphome

#endif
