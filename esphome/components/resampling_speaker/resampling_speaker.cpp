#ifdef USE_ESP32

#include "resampling_speaker.h"

#include "esphome/components/audio/audio_resampler.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace resampling_speaker {

static const UBaseType_t RESAMPLER_TASK_PRIORITY = 1;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const uint32_t TRANSFER_BUFFER_DURATION_MS = 50;
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

void ResamplingSpeaker::setup() {}

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
        if (!this->transfer_buffer_->has_buffered_data()) {
          if ((this->timeout_ms_.has_value() && ((millis() - this->last_seen_data_ms_) > this->timeout_ms_.value())) ||
              this->stop_gracefully_) {
            this->state_ = speaker::STATE_STOPPING;
          }
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
    const size_t ring_buffer_size = MIXER_INPUT_RING_BUFFER_DURATION_MS * this->audio_stream_info_.get_bytes_per_ms();
    if (this->transfer_buffer_.use_count() == 0) {
      this->transfer_buffer_ = audio::AudioSourceTransferBuffer::create(TRANSFER_BUFFER_DURATION_MS *
                                                                        this->audio_stream_info_.get_bytes_per_ms());

      if (this->transfer_buffer_ == nullptr) {
        return ESP_ERR_NO_MEM;
      }
      std::shared_ptr<RingBuffer> temp_ring_buffer;

      if (!this->ring_buffer_.use_count()) {
        temp_ring_buffer = RingBuffer::create(ring_buffer_size);
        this->ring_buffer_ = temp_ring_buffer;
      }

      if (!this->ring_buffer_.use_count()) {
        return ESP_ERR_NO_MEM;
      } else {
        this->transfer_buffer_->set_source(temp_ring_buffer);
      }

      if (this->task_handle_ == nullptr) {
        xTaskCreate(this->resample_task, "resample", TASK_STACK_SIZE, (void *) this, RESAMPLER_TASK_PRIORITY,
                    &this->task_handle_);

        if (this->task_handle_ == nullptr) {
          return ESP_ERR_INVALID_STATE;
        }

        this->task_created_ = true;
      }
    }
  }

  return ESP_OK;
}

void ResamplingSpeaker::stop() { this->state_ = speaker::STATE_STOPPING; }

void ResamplingSpeaker::stop_() {
  this->transfer_buffer_.reset();  // deallocates the transfer buffer
}

void ResamplingSpeaker::finish() { this->stop_gracefully_ = true; }

bool ResamplingSpeaker::has_buffered_data() const { return this->transfer_buffer_->has_buffered_data(); }

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
  // ResamplingSpeaker *this_resampler = (ResamplingSpeaker *) params;

  // // xEventGroupSetBits(this_resampler->event_group_, MixerEventGroupBits::STATE_STARTING);

  // // std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer = audio::AudioSinkTransferBuffer::create(
  // //     TRANSFER_BUFFER_DURATION_MS * this_resampler->audio_stream_info_.value().get_bytes_per_ms());

  // // if (output_transfer_buffer == nullptr) {
  // //   xEventGroupSetBits(this_resampler->event_group_,
  // //                      MixerEventGroupBits::STATE_STOPPED | MixerEventGroupBits::ERR_ESP_NO_MEM);

  // //   this_resampler->task_created_ = false;
  // //   vTaskDelete(nullptr);
  // // }

  // // output_transfer_buffer->set_sink(this_resampler->output_speaker_);

  // // xEventGroupSetBits(this_resampler->event_group_, MixerEventGroupBits::STATE_RUNNING);

  // // while (true) {
  // //   std::shared_ptr<audio::AudioSourceTransferBuffer> primary_transfer_buffer =
  // //       this_resampler->source_speakers_[0]->get_transfer_buffer().lock();
  // //   std::shared_ptr<audio::AudioSourceTransferBuffer> secondary_transfer_buffer =
  // //       this_resampler->source_speakers_[1]->get_transfer_buffer().lock();

  // //   audio::AudioStreamInfo primary_stream_info = this_resampler->source_speakers_[0]->get_audio_stream_info();
  // //   audio::AudioStreamInfo secondary_stream_info = this_resampler->source_speakers_[1]->get_audio_stream_info();

  // //   uint32_t event_group_bits = xEventGroupGetBits(this_resampler->event_group_);
  // //   if (event_group_bits & MixerEventGroupBits::COMMAND_STOP) {
  // //     break;
  // //   }

  // //   output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(TASK_DELAY_MS));

  // //   const uint32_t output_frames_free =
  // //       output_transfer_buffer->free() / this_resampler->audio_stream_info_.value().get_bytes_per_frame();

  // //   uint32_t primary_frames_available = 0;
  // //   if (primary_transfer_buffer.use_count() > 0) {
  // //     this_resampler->source_speakers_[0]->process_data_from_source(0);
  // //     primary_frames_available = primary_transfer_buffer->available() / primary_stream_info.get_bytes_per_frame();
  // //   }

  // //   uint32_t secondary_frames_available = 0;
  // //   if (secondary_transfer_buffer.use_count() > 0) {
  // //     this_resampler->source_speakers_[1]->process_data_from_source(0);
  // //     secondary_frames_available = secondary_transfer_buffer->available() /
  // //     secondary_stream_info.get_bytes_per_frame();
  // //   }

  // //   // Restrict the number of frames available to the amount that the output transfer buffer can store
  // //   primary_frames_available = std::min(primary_frames_available, output_frames_free);
  // //   secondary_frames_available = std::min(secondary_frames_available, output_frames_free);

  // //   if (secondary_frames_available + primary_frames_available > 0) {
  // //     // Copies audio based on frames instead of bytes to avoid ever transferring half a sample or frame

  // //     size_t bytes_written = 0;
  // //     if ((secondary_frames_available > 0) && (primary_frames_available > 0)) {
  // //       // Mix the audio
  // //       uint32_t frames_to_transfer = std::min(secondary_frames_available, primary_frames_available);

  // //       this_resampler->mix_audio_samples_without_clipping(
  // //           reinterpret_cast<int16_t *>(primary_transfer_buffer->get_buffer_start()), primary_stream_info,
  // //           reinterpret_cast<int16_t *>(secondary_transfer_buffer->get_buffer_start()), secondary_stream_info,
  // //           reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
  // //           this_resampler->audio_stream_info_.value(), frames_to_transfer);

  // //       size_t primary_bytes_read = frames_to_transfer * primary_stream_info.get_bytes_per_frame();
  // //       size_t secondary_bytes_read = frames_to_transfer * secondary_stream_info.get_bytes_per_frame();
  // //       bytes_written = frames_to_transfer * this_resampler->audio_stream_info_.value().get_bytes_per_frame();

  // //       primary_transfer_buffer->decrease_buffer_length(primary_bytes_read);
  // //       secondary_transfer_buffer->decrease_buffer_length(secondary_bytes_read);
  // //     } else if (primary_frames_available > 0) {
  // //       size_t bytes_read = 0;
  // //       this_resampler->copy_frames(
  // //           reinterpret_cast<int16_t *>(primary_transfer_buffer->get_buffer_start()), primary_stream_info,
  // //           reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
  // //           this_resampler->audio_stream_info_.value(), primary_frames_available, bytes_read, bytes_written);

  // //       primary_transfer_buffer->decrease_buffer_length(bytes_read);
  // //     } else if (secondary_frames_available > 0) {
  // //       size_t bytes_read = 0;
  // //       this_resampler->copy_frames(
  // //           reinterpret_cast<int16_t *>(secondary_transfer_buffer->get_buffer_start()), secondary_stream_info,
  // //           reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
  // //           this_resampler->audio_stream_info_.value(), secondary_frames_available, bytes_read, bytes_written);

  // //       secondary_transfer_buffer->decrease_buffer_length(bytes_read);
  // //     }

  // //     output_transfer_buffer->increase_buffer_length(bytes_written);
  // //   } else {
  // //     // No audio data available in either buffer
  // //     delay(TASK_DELAY_MS);
  // //   }
  // // }

  // // xEventGroupSetBits(this_resampler->event_group_, MixerEventGroupBits::STATE_STOPPING);

  // // output_transfer_buffer.reset();

  // // xEventGroupSetBits(this_resampler->event_group_, MixerEventGroupBits::STATE_STOPPED);
  // this_resampler->task_created_ = false;
  vTaskDelete(nullptr);
}

}  // namespace resampling_speaker
}  // namespace esphome

#endif
