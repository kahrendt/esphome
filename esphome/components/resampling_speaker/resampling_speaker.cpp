#ifdef USE_ESP32

#include "resampling_speaker.h"

#include "esphome/components/audio/audio_resampler.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace resampling_speaker {

static const UBaseType_t RESAMPLER_TASK_PRIORITY = 1;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 100;
static const uint32_t TRANSFER_BUFFER_DURATION_MS = 100;
static const size_t TASK_DELAY_MS = 25;

static const uint32_t TASK_STACK_SIZE = 3072;

static const char *const TAG = "resampling_speaker";

enum ResamplingEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // stops the mixer task
  STATE_STARTING = (1 << 10),
  STATE_RUNNING = (1 << 11),
  STATE_STOPPING = (1 << 12),
  STATE_STOPPED = (1 << 13),
  ERR_ESP_NO_MEM = (1 << 19),
  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void ResamplingSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
}

void ResamplingSpeaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING: {
      esp_err_t err = this->start_();
      if (err == ESP_OK) {
        this->state_ = speaker::STATE_RUNNING;
        this->stop_gracefully_ = false;
        this->last_seen_data_ms_ = millis();
        this->status_clear_error();
      } else {
        switch (err) {
          case ESP_ERR_NO_MEM:
            this->status_set_error("Failed to start resampler: not enough memory");
            break;
          case ESP_ERR_INVALID_STATE:
            this->status_set_error("Failed to start resampler: resampler task failed to start");
            break;
          default:
            this->status_set_error("Failed to start resampler");
            break;
        }

        this->state_ = speaker::STATE_STOPPING;
      }
      break;
    }
    case speaker::STATE_RUNNING:
      if (this->audio_stream_info_.sample_rate != this->target_sample_rate_) {
        if ((this->timeout_ms_.has_value() && ((millis() - this->last_seen_data_ms_) > this->timeout_ms_.value())) ||
            this->stop_gracefully_) {
          this->state_ = speaker::STATE_STOPPING;
        }

      } else {
        if (!this->output_speaker_->has_buffered_data()) {
          if ((this->timeout_ms_.has_value() && ((millis() - this->last_seen_data_ms_) > this->timeout_ms_.value())) ||
              this->stop_gracefully_) {
            this->state_ = speaker::STATE_STOPPING;
          }
        }
      }

      break;
    case speaker::STATE_STOPPING:
      this->stop_();
      this->stop_gracefully_ = false;
      this->state_ = speaker::STATE_STOPPED;
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

size_t ResamplingSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }

  size_t bytes_written = 0;
  if ((this->output_speaker_->is_running()) && (this->audio_stream_info_.sample_rate == this->target_sample_rate_)) {
    bytes_written = this->output_speaker_->play(data, length, ticks_to_wait);
  } else {
    if (this->ring_buffer_.use_count() == 1) {
      std::shared_ptr<RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
      bytes_written = temp_ring_buffer->write_without_replacement(data, length, ticks_to_wait);
    }
  }

  if (bytes_written > 0) {
    this->last_seen_data_ms_ = millis();
  }
  return bytes_written;
}

void ResamplingSpeaker::start() { this->state_ = speaker::STATE_STARTING; }

esp_err_t ResamplingSpeaker::start_() {
  audio::AudioStreamInfo resampled_stream_info = this->audio_stream_info_;
  resampled_stream_info.sample_rate = this->target_sample_rate_;

  this->output_speaker_->set_audio_stream_info(resampled_stream_info);
  this->output_speaker_->start();

  if (this->audio_stream_info_.sample_rate != this->target_sample_rate_) {
    // we actually have to resample!

    // if (!this->ring_buffer_.use_count()) {
    //   const size_t ring_buffer_size = MIXER_INPUT_RING_BUFFER_DURATION_MS *
    //   this->audio_stream_info_.get_bytes_per_ms(); this->ring_buffer_ = RingBuffer::create(ring_buffer_size);
    // }

    // if (!this->ring_buffer_.use_count()) {
    //   return ESP_ERR_NO_MEM;
    // }

    if (this->task_handle_ == nullptr) {
      xTaskCreate(this->resample_task, "resample", TASK_STACK_SIZE, (void *) this, RESAMPLER_TASK_PRIORITY,
                  &this->task_handle_);

      if (this->task_handle_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
      }

      this->task_created_ = true;
    }
  }

  return ESP_OK;
}

void ResamplingSpeaker::stop() { this->state_ = speaker::STATE_STOPPING; }

void ResamplingSpeaker::stop_() {
  this->ring_buffer_.reset();  // deallocates the transfer buffer
}

void ResamplingSpeaker::finish() { this->stop_gracefully_ = true; }

bool ResamplingSpeaker::has_buffered_data() const { return (this->ring_buffer_.lock()->available() > 0); }

void ResamplingSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->output_speaker_->set_mute_state(mute_state);
}

void ResamplingSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->output_speaker_->set_volume(volume);
}

// void ResamplingSpeaker::loop() {
//   uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

//   if (event_group_bits & MixerEventGroupBits::STATE_STARTING) {
//     ESP_LOGD(TAG, "Starting Mixer");
//     xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_STARTING);
//   }
//   if (event_group_bits & MixerEventGroupBits::ERR_ESP_NO_MEM) {
//     this->status_set_error("Failed to allocate the mixer's internal buffer");
//     xEventGroupClearBits(this->event_group_, MixerEventGroupBits::ERR_ESP_NO_MEM);
//   }
//   if (event_group_bits & MixerEventGroupBits::STATE_RUNNING) {
//     ESP_LOGD(TAG, "Started Mixer");
//     this->status_clear_error();
//     xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_RUNNING);
//   }
//   if (event_group_bits & MixerEventGroupBits::STATE_STOPPING) {
//     ESP_LOGD(TAG, "Stopping Mixer");
//     xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_STOPPING);
//   }
//   if (event_group_bits & MixerEventGroupBits::STATE_STOPPED) {
//     if (!this->task_created_) {
//       ESP_LOGD(TAG, "Stopped Mixer");
//       xEventGroupClearBits(this->event_group_, MixerEventGroupBits::ALL_BITS);
//       this->task_handle_ = nullptr;
//     }
//   }

//   if (this->task_handle_ != nullptr) {
//     if (this->source_speakers_[0]->is_stopped() && this->source_speakers_[1]->is_stopped()) {
//       this->stop();
//     }
//   }
// }

// esp_err_t SpeakerMixer::start(audio::AudioStreamInfo &stream_info) {
//   if (!this->audio_stream_info_.has_value()) {
//     if (stream_info.bits_per_sample != 16) {
//       // Audio streams that don't have 16 bits per sample are not supported
//       return ESP_ERR_NOT_SUPPORTED;
//     }

//     this->audio_stream_info_ = stream_info;
//     this->audio_stream_info_.value().channels = OUTPUT_CHANNELS;
//     this->output_speaker_->set_audio_stream_info(this->audio_stream_info_.value());
//   } else {
//     if (stream_info.sample_rate != this->audio_stream_info_.value().sample_rate) {
//       // The two audio streams must have the same sample rate to mix properly
//       return ESP_ERR_INVALID_ARG;
//     }
//   }

//   if (this->task_handle_ == nullptr) {
//     xTaskCreate(this->audio_mixer_task, "mixer", TASK_STACK_SIZE, (void *) this, MIXER_TASK_PRIORITY,
//                 &this->task_handle_);

//     if (this->task_handle_ == nullptr) {
//       return ESP_ERR_INVALID_STATE;
//     }

//     this->task_created_ = true;
//   }

//   return ESP_OK;
// }

// void SpeakerMixer::stop() { xEventGroupSetBits(this->event_group_, MixerEventGroupBits::COMMAND_STOP); }

void ResamplingSpeaker::resample_task(void *params) {
  ResamplingSpeaker *this_resampler = (ResamplingSpeaker *) params;

  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STARTING);

  audio::AudioStreamInfo resampled_stream_info = this_resampler->audio_stream_info_;
  resampled_stream_info.sample_rate = this_resampler->target_sample_rate_;

  std::unique_ptr<audio::AudioResampler> resampler = std::make_unique<audio::AudioResampler>(
      TRANSFER_BUFFER_DURATION_MS * this_resampler->audio_stream_info_.get_bytes_per_ms(),
      TRANSFER_BUFFER_DURATION_MS * resampled_stream_info.get_bytes_per_ms());

  esp_err_t err = resampler->start(this_resampler->audio_stream_info_, this_resampler->target_sample_rate_);

  // TODO: Verify err is ESP_OK

  {
    std::shared_ptr<RingBuffer> temp_ring_buffer =
        RingBuffer::create(MIXER_INPUT_RING_BUFFER_DURATION_MS * this_resampler->audio_stream_info_.get_bytes_per_ms());

    if (temp_ring_buffer.use_count() == 0) {
      // TODO: Handle error state
    }

    this_resampler->ring_buffer_ = temp_ring_buffer;
    resampler->add_source(this_resampler->ring_buffer_);
  }

  this_resampler->output_speaker_->set_audio_stream_info(resampled_stream_info);
  resampler->add_sink(this_resampler->output_speaker_);

  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_RUNNING);

  while (true) {
    uint32_t event_bits = xEventGroupGetBits(this_resampler->event_group_);

    if (event_bits & ResamplingEventGroupBits::COMMAND_STOP) {
      break;
    }

    // Stop gracefully if the decoder is done
    audio::AudioResamplerState resampler_state = resampler->resample(false);

    if (resampler_state == audio::AudioResamplerState::FINISHED) {
      break;
    } else if (resampler_state == audio::AudioResamplerState::FAILED) {
      // TODO: probably not the correct error
      xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_NO_MEM);
      break;
    }
  }

  resampler.reset();
  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STOPPED);
  vTaskDelete(nullptr);
}

}  // namespace resampling_speaker
}  // namespace esphome

#endif
