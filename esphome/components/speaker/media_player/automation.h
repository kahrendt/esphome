#pragma once

#include "speaker_media_player.h"

#ifdef USE_ESP_IDF

#include "esphome/components/audio/audio.h"
#include "esphome/core/automation.h"

namespace esphome {
namespace speaker {

template<typename... Ts> class PlayOnDeviceMediaAction : public Action<Ts...>, public Parented<SpeakerMediaPlayer> {
  TEMPLATABLE_VALUE(audio::AudioFile *, audio_file)
  TEMPLATABLE_VALUE(bool, announcement)
  void play(Ts... x) override {
    this->parent_->play_file(this->audio_file_.value(x...), this->announcement_.value(x...));
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
