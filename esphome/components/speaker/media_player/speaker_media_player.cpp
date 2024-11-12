#include "speaker_media_player.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

namespace esphome {
namespace speaker {

// Framework:
//  - Media player that can handle two streams; one for media and one for announcements
//    - If played together, they are mixed with the announcement stream staying at full volume
//    - The media audio is scaled, if necessary, to avoid clipping when mixing an announcement stream
//    - The media audio can be further ducked via the ``set_ducking_reduction`` function
//  - Each stream is handled by an ``AudioPipeline`` object with three parts/tasks
//    - ``AudioReader`` handles reading from an HTTP source or from a PROGMEM flash set at compile time
//    - ``AudioDecoder`` handles decoding the audio file. All formats are limited to two channels and 16 bits per sample
//      - FLAC
//      - WAV
//      - MP3 (based on the libhelix decoder - a random mp3 file may be incompatible)
//    - ``AudioResampler`` handles converting the sample rate to the configured output sample rate and converting mono
//      to stereo
//      - The quality is not good, and it is slow! Please use audio at the configured sample rate to avoid these issues
//    - Each task will always run once started, but they will not doing anything until they are needed
//    - FreeRTOS Event Groups make up the inter-task communication
//    - The ``AudioPipeline`` sets up an output ring buffer for the Reader and Decoder parts. The next part/task
//      automatically pulls from the previous ring buffer
//  - The streams are mixed together in the ``AudioMixer`` task
//    - Each stream has a corresponding input buffer that the ``AudioResampler`` feeds directly
//    - Pausing the media stream is done here
//    - Media stream ducking is done here
//    - The output ring buffer feeds the configured speaker the audio directly
//  - Generic media player commands are received by the ``control`` function. The commands are added to the
//    ``media_control_command_queue_`` to be processed in the component's loop
//    - Local file play back is initiatied with ``play_file`` and adds it to the ``media_control_command_queue_``
//    - Starting a stream intializes the appropriate pipeline or stops it if it is already running
//    - Volume and mute commands are achieved by the ``mute``, ``unmute``, ``set_volume`` functions. The speaker
//      component handles the implementation details.
//      - Volume commands are ignored if the media control queue is full to avoid crashing when the track wheel is spun
//      fast
//    - Pausing is sent to the ``AudioMixer`` task. It only effects the media stream.
//  - The components main loop performs housekeeping:
//    - It reads the media control queue and processes it directly
//    - It watches the state of speaker and mixer tasks
//    - It determines the overall state of the media player by considering the state of each pipeline
//      - announcement playback takes highest priority
//  - All logging happens in the main loop task to reduce task stack memory usage.

static const size_t QUEUE_LENGTH = 20;

static const uint8_t NUMBER_OF_CHANNELS = 2;  // Hard-coded expectation of stereo (2 channel) audio

static const UBaseType_t MEDIA_PIPELINE_TASK_PRIORITY = 1;
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
static const UBaseType_t ANNOUNCEMENT_PIPELINE_TASK_PRIORITY = 1;
#endif

static const size_t TASK_DELAY_MS = 10;

static const float FIRST_BOOT_DEFAULT_VOLUME = 0.5f;

static const char *const TAG = "speaker_media_player";

void SpeakerMediaPlayer::setup() {
  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  this->media_control_command_queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(MediaCallCommand));

  this->pref_ = global_preferences->make_preference<VolumeRestoreState>(this->get_object_id_hash());

  VolumeRestoreState volume_restore_state;
  if (this->pref_.load(&volume_restore_state)) {
    this->set_volume_(volume_restore_state.volume);
    this->set_mute_state_(volume_restore_state.is_muted);
  } else {
    this->set_volume_(FIRST_BOOT_DEFAULT_VOLUME);
    this->set_mute_state_(false);
  }

#ifdef USE_OTA
  ota::get_global_ota_callback()->add_on_state_callback(
      [this](ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
        if (state == ota::OTA_STARTED) {
          if (this->media_pipeline_ != nullptr) {
            this->media_pipeline_->suspend_tasks();
          }
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
          if (this->announcement_pipeline_ != nullptr) {
            this->announcement_pipeline_->suspend_tasks();
          }
#endif
        } else if (state == ota::OTA_ERROR) {
          if (this->media_pipeline_ != nullptr) {
            this->media_pipeline_->resume_tasks();
          }
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
          if (this->announcement_pipeline_ != nullptr) {
            this->announcement_pipeline_->resume_tasks();
          }
#endif
        }
      });
#endif

  ESP_LOGI(TAG, "Set up speaker media player");
}

esp_err_t SpeakerMediaPlayer::start_pipeline_(AudioPipelineType type, bool url) {
  esp_err_t err = ESP_OK;

  if (this->media_speaker_ != nullptr) {
    audio::AudioStreamInfo audio_stream_info;
    audio_stream_info.channels = 2;
    audio_stream_info.bits_per_sample = 16;
    audio_stream_info.sample_rate = this->sample_rate_;

    this->announcement_speaker_->set_audio_stream_info(audio_stream_info);
    this->media_speaker_->set_audio_stream_info(audio_stream_info);
  }

#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  if (type == AudioPipelineType::MEDIA) {
#endif
    if (this->media_pipeline_ == nullptr) {
      this->media_pipeline_ = make_unique<AudioPipeline>(this->media_speaker_);
    }
    if (url) {
      err = this->media_pipeline_->start(this->media_url_.value(), this->sample_rate_, "media",
                                         MEDIA_PIPELINE_TASK_PRIORITY);
    } else {
      err = this->media_pipeline_->start(this->media_file_.value(), this->sample_rate_, "media",
                                         MEDIA_PIPELINE_TASK_PRIORITY);
    }

#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  } else if (type == AudioPipelineType::ANNOUNCEMENT) {
    if (this->announcement_pipeline_ == nullptr) {
      this->announcement_pipeline_ = make_unique<AudioPipeline>(this->announcement_speaker_);
    }

    if (url) {
      err = this->announcement_pipeline_->start(this->announcement_url_.value(), this->sample_rate_, "ann",
                                                ANNOUNCEMENT_PIPELINE_TASK_PRIORITY);
    } else {
      err = this->announcement_pipeline_->start(this->announcement_file_.value(), this->sample_rate_, "ann",
                                                ANNOUNCEMENT_PIPELINE_TASK_PRIORITY);
    }
  }
#endif

  return err;
}

void SpeakerMediaPlayer::watch_media_commands_() {
  MediaCallCommand media_command;
  esp_err_t err = ESP_OK;

  if (xQueueReceive(this->media_control_command_queue_, &media_command, 0) == pdTRUE) {
    if (media_command.new_url.has_value() && media_command.new_url.value()) {
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
      if (media_command.announce.has_value() && media_command.announce.value()) {
        err = this->start_pipeline_(AudioPipelineType::ANNOUNCEMENT, true);
      } else {
#endif
        err = this->start_pipeline_(AudioPipelineType::MEDIA, true);
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
      }
#endif
      this->is_paused_ = false;
    }

    if (media_command.new_file.has_value() && media_command.new_file.value()) {
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
      if (media_command.announce.has_value() && media_command.announce.value()) {
        err = this->start_pipeline_(AudioPipelineType::ANNOUNCEMENT, false);
      } else {
#endif
        err = this->start_pipeline_(AudioPipelineType::MEDIA, false);
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
      }
#endif
      this->is_paused_ = false;
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error starting the audio pipeline: %s", esp_err_to_name(err));
      this->status_set_error();
    } else {
      this->status_clear_error();
    }

    if (media_command.volume.has_value()) {
      this->set_volume_(media_command.volume.value());
      this->publish_state();
    }

    if (media_command.command.has_value()) {
      switch (media_command.command.value()) {
        case media_player::MEDIA_PLAYER_COMMAND_PLAY:
          if ((this->media_pipeline_ != nullptr) && (this->is_paused_)) {
            this->media_pipeline_->set_pause_state(false);
          }
          this->is_paused_ = false;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
          if ((this->media_pipeline_ != nullptr) && (!this->is_paused_)) {
            this->media_pipeline_->set_pause_state(true);
          }
          this->is_paused_ = true;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_STOP:
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
          if (media_command.announce.has_value() && media_command.announce.value()) {
            if (this->announcement_pipeline_ != nullptr) {
              this->announcement_pipeline_->stop();
            }
          } else {
#endif
            if (this->media_pipeline_ != nullptr) {
              this->media_pipeline_->stop();
            }
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
          }
#endif
          break;
        case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
          if (this->media_pipeline_ != nullptr) {
            if (this->is_paused_) {
              this->media_pipeline_->set_pause_state(false);
              this->is_paused_ = false;
            } else {
              this->media_pipeline_->set_pause_state(true);
              this->is_paused_ = true;
            }
          }
          break;
        case media_player::MEDIA_PLAYER_COMMAND_MUTE: {
          this->set_mute_state_(true);

          this->publish_state();
          break;
        }
        case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
          this->set_mute_state_(false);
          this->publish_state();
          break;
        case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP:
          this->set_volume_(std::min(1.0f, this->volume + this->volume_increment_));
          this->publish_state();
          break;
        case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN:
          this->set_volume_(std::max(0.0f, this->volume - this->volume_increment_));
          this->publish_state();
          break;
        default:
          break;
      }
    }
  }
}

void SpeakerMediaPlayer::loop() {
  this->watch_media_commands_();

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  if (this->media_pipeline_ != nullptr) {
    static uint32_t decoded_duration = 0;
    this->media_pipeline_state_ = this->media_pipeline_->get_state();
    decoded_duration = this->media_pipeline_->get_playback_ms();
  }

  if (this->media_pipeline_state_ == AudioPipelineState::ERROR_READING) {
    ESP_LOGE(TAG, "The media pipeline's file reader encountered an error.");
  } else if (this->media_pipeline_state_ == AudioPipelineState::ERROR_DECODING) {
    ESP_LOGE(TAG, "The media pipeline's audio decoder encountered an error.");
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  } else if (this->media_pipeline_state_ == AudioPipelineState::ERROR_RESAMPLING) {
    ESP_LOGE(TAG, "The media pipeline's audio resampler encountered an error.");
#endif
  }

#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  if (this->announcement_pipeline_ != nullptr)
    this->announcement_pipeline_state_ = this->announcement_pipeline_->get_state();

  if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_READING) {
    ESP_LOGE(TAG, "The announcement pipeline's file reader encountered an error.");
  } else if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_DECODING) {
    ESP_LOGE(TAG, "The announcement pipeline's audio decoder encountered an error.");
  } else if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_RESAMPLING) {
    ESP_LOGE(TAG, "The announcement pipeline's audio resampler encountered an error.");
  }

  if (this->announcement_pipeline_state_ != AudioPipelineState::STOPPED) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  } else {
#endif
    if (this->media_pipeline_state_ == AudioPipelineState::STOPPED) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    } else if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
    }
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  }
#endif
  if (this->state != old_state) {
    this->publish_state();
    ESP_LOGD(TAG, "State changed to %s", media_player::media_player_state_to_string(this->state));
  }
}

void SpeakerMediaPlayer::play_file(audio::AudioFile *media_file, bool announcement) {
  if (!this->is_ready()) {
    // Ignore any commands sent before the media player is setup
    return;
  }

  MediaCallCommand media_command;

  media_command.new_file = true;
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  if (announcement) {
    this->announcement_file_ = media_file;
    media_command.announce = true;
  } else {
#endif
    this->media_file_ = media_file;
    media_command.announce = false;
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  }
#endif
  xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
}

void SpeakerMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!this->is_ready()) {
    // Ignore any commands sent before the media player is setup
    return;
  }

  MediaCallCommand media_command;

  if (call.get_announcement().has_value() && call.get_announcement().value()) {
    media_command.announce = true;
  } else {
    media_command.announce = false;
  }

  if (call.get_media_url().has_value()) {
    std::string new_uri = call.get_media_url().value();

    media_command.new_url = true;
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
    if (call.get_announcement().has_value() && call.get_announcement().value()) {
      this->announcement_url_ = new_uri;
    } else {
#endif
      this->media_url_ = new_uri;
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
    }
#endif
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }

  if (call.get_volume().has_value()) {
    media_command.volume = call.get_volume().value();
    // Wait 0 ticks for queue to be free, volume sets aren't that important!
    xQueueSend(this->media_control_command_queue_, &media_command, 0);
    return;
  }

  if (call.get_command().has_value()) {
    media_command.command = call.get_command().value();
    TickType_t ticks_to_wait = portMAX_DELAY;
    if ((call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP) ||
        (call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN)) {
      ticks_to_wait = 0;  // Wait 0 ticks for queue to be free, volume sets aren't that important!
    }
    xQueueSend(this->media_control_command_queue_, &media_command, ticks_to_wait);
    return;
  }
}

media_player::MediaPlayerTraits SpeakerMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
#ifdef USE_SPEAKER_MEDIA_PLAYER_DUAL_PIPELINE
  traits.get_supported_formats().push_back(
      media_player::MediaPlayerSupportedFormat{.format = "flac",
                                               .sample_rate = this->sample_rate_,
                                               .num_channels = 2,
                                               .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_DEFAULT,
                                               .sample_bytes = 2});
  traits.get_supported_formats().push_back(
      media_player::MediaPlayerSupportedFormat{.format = "flac",
                                               .sample_rate = this->sample_rate_,
                                               .num_channels = 1,
                                               .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT,
                                               .sample_bytes = 2});
#else
  traits.set_supports_pause(false);
  traits.get_supported_formats().push_back(
      media_player::MediaPlayerSupportedFormat{.format = "wav",
                                               .sample_rate = this->sample_rate_,
                                               .num_channels = 1,
                                               .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_DEFAULT,
                                               .sample_bytes = 2});
  traits.get_supported_formats().push_back(
      media_player::MediaPlayerSupportedFormat{.format = "wav",
                                               .sample_rate = this->sample_rate_,
                                               .num_channels = 1,
                                               .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT,
                                               .sample_bytes = 2});
#endif

  return traits;
};

void SpeakerMediaPlayer::save_volume_restore_state_() {
  VolumeRestoreState volume_restore_state;
  volume_restore_state.volume = this->volume;
  volume_restore_state.is_muted = this->is_muted_;
  this->pref_.save(&volume_restore_state);
}

void SpeakerMediaPlayer::set_mute_state_(bool mute_state) {
  this->media_speaker_->set_mute_state(mute_state);

  bool old_mute_state = this->is_muted_;
  this->is_muted_ = mute_state;

  this->save_volume_restore_state_();

  if (old_mute_state != mute_state) {
    if (mute_state) {
      this->defer([this]() { this->mute_trigger_->trigger(); });
    } else {
      this->defer([this]() { this->unmute_trigger_->trigger(); });
    }
  }
}

void SpeakerMediaPlayer::set_volume_(float volume, bool publish) {
  // Remap the volume to fit with in the configured limits
  float bounded_volume = remap<float, float>(volume, 0.0f, 1.0f, this->volume_min_, this->volume_max_);

  this->media_speaker_->set_volume(bounded_volume);

  if (publish) {
    this->volume = volume;
    this->save_volume_restore_state_();
  }

  this->defer([this, volume]() { this->volume_trigger_->trigger(volume); });
}

}  // namespace speaker
}  // namespace esphome

#endif
