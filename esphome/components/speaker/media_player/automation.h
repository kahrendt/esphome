#pragma once

#ifdef USE_ESP_IDF

#include "esphome/core/automation.h"
#include "speaker_media_player.h"

namespace esphome {
namespace speaker {

template<typename... Ts> class DuckingSetAction : public Action<Ts...>, public Parented<SpeakerMediaPlayer> {
  TEMPLATABLE_VALUE(uint8_t, decibel_reduction)
  TEMPLATABLE_VALUE(float, duration)
  void play(Ts... x) override {
    this->parent_->set_ducking_reduction(this->decibel_reduction_.value(x...), this->duration_.value(x...));
  }
};

template<typename... Ts> class PlayLocalMediaAction : public Action<Ts...>, public Parented<SpeakerMediaPlayer> {
  TEMPLATABLE_VALUE(MediaFile *, media_file)
  TEMPLATABLE_VALUE(bool, announcement)
  void play(Ts... x) override {
    this->parent_->play_file(this->media_file_.value(x...), this->announcement_.value(x...));
  }
};

template<typename... Ts> class StopStreamAction : public Action<Ts...>, public Parented<SpeakerMediaPlayer> {
  TEMPLATABLE_VALUE(AudioPipelineType, pipeline_type)
  void play(Ts... x) override {
    bool announcement = false;
    if (this->pipeline_type_.value(x...) == AudioPipelineType::ANNOUNCEMENT) {
      announcement = true;
    }
    this->parent_->make_call()
        .set_command(media_player::MediaPlayerCommand::MEDIA_PLAYER_COMMAND_STOP)
        .set_announcement(announcement)
        .perform();
  }
};

}  // namespace speaker
}  // namespace esphome

#endif
