#ifdef USE_ESP32

#include "speaker_mixer.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <dsp.h>  // esp_audio_libs

namespace esphome {
namespace speaker_mixer {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const uint32_t TRANSFER_BUFFER_DURATION_MS = 50;
static const size_t TASK_DELAY_MS = 25;

static const uint32_t TASK_STACK_SIZE = 3072;

static const int16_t MAX_AUDIO_SAMPLE_VALUE = INT16_MAX;
static const int16_t MIN_AUDIO_SAMPLE_VALUE = INT16_MIN;

static const uint8_t OUTPUT_CHANNELS = 2;

static const char *const TAG = "speaker_mixer";

// Gives the Q15 fixed point scaling factor to reduce by 0 dB, 1dB, ..., 50 dB
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> DECIBEL_REDUCTION_TABLE = {
    32767, 29201, 26022, 23189, 20665, 18415, 16410, 14624, 13032, 11613, 10349, 9222, 8218, 7324, 6527, 5816, 5183,
    4619,  4116,  3668,  3269,  2913,  2596,  2313,  2061,  1837,  1637,  1459,  1300, 1158, 1032, 920,  820,  731,
    651,   580,   517,   461,   411,   366,   326,   291,   259,   231,   206,   183,  163,  146,  130,  116,  103};

enum MixerEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // stops the mixer task
  STATE_STARTING = (1 << 10),
  STATE_RUNNING = (1 << 11),
  STATE_STOPPING = (1 << 12),
  STATE_STOPPED = (1 << 13),
  ERR_ESP_NO_MEM = (1 << 19),
  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void SourceSpeaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING: {
      esp_err_t err = this->start_();
      if (err == ESP_OK) {
        this->state_ = speaker::STATE_RUNNING;
        this->stop_gracefully_ = false;
        this->status_clear_error();
      } else {
        switch (err) {
          case ESP_ERR_NO_MEM:
            this->status_set_error("Failed to start mixer: not enough memory");
            break;
          case ESP_ERR_NOT_SUPPORTED:
            this->status_set_error("Failed to start mixer: unsupported bits per sample");
            break;
          case ESP_ERR_INVALID_ARG:
            this->status_set_error("Failed to start mixer: audio stream isn't compatable with the other audio stream");
            break;
          case ESP_ERR_INVALID_STATE:
            this->status_set_error("Failed to start mixer: mixer task failed to start");
            break;
          default:
            this->status_set_error("Failed to start mixer");
            break;
        }

        this->state_ = speaker::STATE_STOPPING;
      }
      break;
    }
    case speaker::STATE_RUNNING:
      if (!this->transfer_buffer_->has_buffered_data()) {
        if ((this->timeout_ms_.has_value() && ((millis() - this->last_seen_data_ms_) > this->timeout_ms_.value())) ||
            this->stop_gracefully_) {
          this->state_ = speaker::STATE_STOPPING;
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

size_t SourceSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }
  size_t bytes_written = 0;
  if (this->ring_buffer_.use_count() == 1) {
    std::shared_ptr<RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    bytes_written = temp_ring_buffer->write_without_replacement(data, length, ticks_to_wait);
    if (bytes_written > 0) {
      this->last_seen_data_ms_ = millis();
    }
  }
  return bytes_written;
}

void SourceSpeaker::start() { this->state_ = speaker::STATE_STARTING; }

esp_err_t SourceSpeaker::start_() {
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
  }

  return this->parent_->start(this->audio_stream_info_);
}

void SourceSpeaker::stop() { this->state_ = speaker::STATE_STOPPING; }

void SourceSpeaker::stop_() {
  this->transfer_buffer_.reset();  // deallocates the transfer buffer
}

void SourceSpeaker::finish() { this->stop_gracefully_ = true; }

bool SourceSpeaker::has_buffered_data() const { return this->transfer_buffer_->has_buffered_data(); }

void SourceSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->parent_->get_output_speaker()->set_mute_state(mute_state);
}

void SourceSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->parent_->get_output_speaker()->set_volume(volume);
}

size_t SourceSpeaker::process_data_from_source(TickType_t ticks_to_wait) {
  if (!this->transfer_buffer_.use_count()) {
    return 0;
  }
  const size_t current_length = this->transfer_buffer_->available();

  size_t bytes_read = this->transfer_buffer_->transfer_data_from_source(ticks_to_wait);

  if (bytes_read > 0) {
    size_t samples_to_duck = bytes_read / this->audio_stream_info_.get_bytes_per_sample();
    if (samples_to_duck > 0) {
      int16_t *current_buffer =
          reinterpret_cast<int16_t *>(this->transfer_buffer_->get_buffer_start() + current_length);

      this->duck_samples(current_buffer, samples_to_duck, this->current_ducking_db_reduction_,
                         this->ducking_transition_samples_remaining_, this->samples_per_ducking_step_,
                         this->db_change_per_ducking_step_);
    }
  }

  return bytes_read;
}

void SourceSpeaker::apply_ducking(uint8_t decibel_reduction, uint32_t duration) {
  if (this->target_ducking_db_reduction_ != decibel_reduction) {
    this->current_ducking_db_reduction_ = this->target_ducking_db_reduction_;

    this->target_ducking_db_reduction_ = decibel_reduction;

    uint8_t total_ducking_steps = 0;
    if (this->target_ducking_db_reduction_ > this->current_ducking_db_reduction_) {
      // The dB reduction level is increasing (which results in quieter audio)
      total_ducking_steps = this->target_ducking_db_reduction_ - this->current_ducking_db_reduction_ - 1;
      this->db_change_per_ducking_step_ = 1;
    } else {
      // The dB reduction level is decreasing (which results in louder audio)
      total_ducking_steps = this->current_ducking_db_reduction_ - this->target_ducking_db_reduction_ - 1;
      this->db_change_per_ducking_step_ = -1;
    }
    if ((duration > 0) && (total_ducking_steps > 0)) {
      this->ducking_transition_samples_remaining_ = duration * this->audio_stream_info_.get_samples_per_ms();

      this->samples_per_ducking_step_ = this->ducking_transition_samples_remaining_ / total_ducking_steps;
      this->ducking_transition_samples_remaining_ =
          this->samples_per_ducking_step_ * total_ducking_steps;  // Adjust for integer division rounding

      this->current_ducking_db_reduction_ += this->db_change_per_ducking_step_;
    } else {
      this->ducking_transition_samples_remaining_ = 0;
      this->current_ducking_db_reduction_ = this->target_ducking_db_reduction_;
    }
  }
}

void SourceSpeaker::duck_samples(int16_t *input_buffer, uint32_t input_samples_to_duck,
                                 int8_t &current_ducking_db_reduction, size_t &ducking_transition_samples_remaining,
                                 size_t samples_per_ducking_step, int8_t db_change_per_ducking_step) {
  if (ducking_transition_samples_remaining > 0) {
    // Ducking level is still transitioning

    // Take the ceiling of secondary_samples_to_duck/samples_per_ducking_step
    size_t ducking_steps_in_batch =
        input_samples_to_duck / samples_per_ducking_step + (input_samples_to_duck % samples_per_ducking_step != 0);

    for (size_t i = 0; i < ducking_steps_in_batch; ++i) {
      uint32_t samples_left_in_step = ducking_transition_samples_remaining % samples_per_ducking_step;

      if (samples_left_in_step == 0) {
        samples_left_in_step = samples_per_ducking_step;
      }

      size_t samples_to_duck = std::min(input_samples_to_duck, samples_left_in_step);
      samples_to_duck = std::min(samples_to_duck, ducking_transition_samples_remaining);

      // Ensure we only point to valid index in the Q15 scaling factor table
      uint8_t safe_db_reduction_index =
          std::clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
      int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

      audio::scale_audio_samples(input_buffer, input_buffer, q15_scale_factor, samples_to_duck);

      if (samples_left_in_step - samples_to_duck == 0) {
        // After scaling the current samples, we are ready to transition to the next step
        current_ducking_db_reduction += db_change_per_ducking_step;
      }

      input_buffer += samples_to_duck;
      ducking_transition_samples_remaining -= samples_to_duck;
      input_samples_to_duck -= samples_to_duck;
    }
  }

  if ((current_ducking_db_reduction > 0) && (input_samples_to_duck > 0)) {
    // We still need to apply ducking, but we are not in the middle of a transition step

    uint8_t safe_db_reduction_index =
        std::clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
    int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

    audio::scale_audio_samples(input_buffer, input_buffer, q15_scale_factor, input_samples_to_duck);
  }
}

void SpeakerMixer::setup() {
  this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
}

void SpeakerMixer::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & MixerEventGroupBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting Mixer");
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_STARTING);
  }
  if (event_group_bits & MixerEventGroupBits::ERR_ESP_NO_MEM) {
    this->status_set_error("Failed to allocate the mixer's internal buffer");
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::ERR_ESP_NO_MEM);
  }
  if (event_group_bits & MixerEventGroupBits::STATE_RUNNING) {
    ESP_LOGD(TAG, "Started Mixer");
    this->status_clear_error();
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_RUNNING);
  }
  if (event_group_bits & MixerEventGroupBits::STATE_STOPPING) {
    ESP_LOGD(TAG, "Stopping Mixer");
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_STOPPING);
  }
  if (event_group_bits & MixerEventGroupBits::STATE_STOPPED) {
    if (!this->task_created_) {
      ESP_LOGD(TAG, "Stopped Mixer");
      xEventGroupClearBits(this->event_group_, MixerEventGroupBits::ALL_BITS);
      this->task_handle_ = nullptr;
    }
  }

  if (this->task_handle_ != nullptr) {
    if (this->source_speakers_[0]->is_stopped() && this->source_speakers_[1]->is_stopped()) {
      this->stop();
    }
  }
}

esp_err_t SpeakerMixer::start(audio::AudioStreamInfo &stream_info) {
  if (!this->audio_stream_info_.has_value()) {
    if (stream_info.bits_per_sample != 16) {
      // Audio streams that don't have 16 bits per sample are not supported
      return ESP_ERR_NOT_SUPPORTED;
    }

    this->audio_stream_info_ = stream_info;
    this->audio_stream_info_.value().channels = OUTPUT_CHANNELS;
    this->output_speaker_->set_audio_stream_info(this->audio_stream_info_.value());
  } else {
    if (stream_info.sample_rate != this->audio_stream_info_.value().sample_rate) {
      // The two audio streams must have the same sample rate to mix properly
      return ESP_ERR_INVALID_ARG;
    }
  }

  if (this->task_handle_ == nullptr) {
    xTaskCreate(this->audio_mixer_task, "mixer", TASK_STACK_SIZE, (void *) this, MIXER_TASK_PRIORITY,
                &this->task_handle_);

    if (this->task_handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }

    this->task_created_ = true;
  }

  return ESP_OK;
}

void SpeakerMixer::stop() { xEventGroupSetBits(this->event_group_, MixerEventGroupBits::COMMAND_STOP); }

void SpeakerMixer::copy_frames(int16_t *input_buffer, audio::AudioStreamInfo input_stream_info, int16_t *output_buffer,
                               audio::AudioStreamInfo output_stream_info, uint32_t frames_to_transfer,
                               size_t &bytes_read, size_t &bytes_written) {
  uint8_t input_channels = input_stream_info.channels;
  uint8_t output_channels = output_stream_info.channels;
  const uint8_t max_input_channel_index = input_channels - 1;

  if (input_channels == output_channels) {
    size_t bytes_to_copy = frames_to_transfer * input_stream_info.get_bytes_per_frame();
    memcpy(output_buffer, input_buffer, bytes_to_copy);
    bytes_read = bytes_to_copy;
    bytes_written = bytes_to_copy;

    return;
  }

  for (size_t frame_index = 0; frame_index < frames_to_transfer; ++frame_index) {
    for (uint8_t output_channel_index = 0; output_channel_index < output_channels; ++output_channel_index) {
      uint8_t input_channel_index = std::min(output_channel_index, max_input_channel_index);
      output_buffer[output_channels * frame_index + output_channel_index] =
          input_buffer[input_channels * frame_index + input_channel_index];
    }
  }
  bytes_read = frames_to_transfer * input_stream_info.get_bytes_per_frame();
  bytes_written = frames_to_transfer * output_stream_info.get_bytes_per_frame();
}

void SpeakerMixer::mix_audio_samples_without_clipping(int16_t *primary_buffer,
                                                      audio::AudioStreamInfo primary_stream_info,
                                                      int16_t *secondary_buffer,
                                                      audio::AudioStreamInfo secondary_stream_info,
                                                      int16_t *output_buffer, audio::AudioStreamInfo output_stream_info,
                                                      size_t frames_to_mix) {
  // We first test adding the two clips samples together and check for any clipping
  // We want the primary volume to be consistent, regardless if secondary is playing or not
  // If there is clipping, we determine what factor we need to multiply that secondary sample by to avoid it
  // We take the smallest factor necessary for all the samples so the secondary volume is consistent on this batch
  // of samples
  // Note: This may not be the best approach. Adding 2 audio samples together makes both sound louder, even if
  // we are not clipping. As a result, the mixed primary will sound louder (by around 3dB if the audio
  // streams are independent?) than if it were by itself.

  const uint8_t primary_channels = primary_stream_info.channels;
  const uint8_t secondary_channels = secondary_stream_info.channels;
  const uint8_t output_channels = output_stream_info.channels;

  const uint8_t max_primary_channel_index = primary_channels - 1;
  const uint8_t max_secondary_channel_index = secondary_channels - 1;

  int16_t q15_scaling_factor = MAX_AUDIO_SAMPLE_VALUE;

  for (uint32_t frames_index = 0; frames_index < frames_to_mix; ++frames_index) {
    for (uint8_t output_channel_index = 0; output_channel_index < output_channels; ++output_channel_index) {
      const ssize_t secondary_channel_index = std::min(output_channel_index, max_secondary_channel_index);
      const int32_t secondary_sample = secondary_buffer[frames_index * secondary_channels + secondary_channel_index];

      const ssize_t primary_channel_index = std::min(output_channel_index, max_primary_channel_index);
      const int32_t primary_sample =
          static_cast<int32_t>(primary_buffer[frames_index * primary_channels + primary_channel_index]);

      const int32_t added_sample = secondary_sample + primary_sample;

      if ((added_sample > MAX_AUDIO_SAMPLE_VALUE) || (added_sample < MIN_AUDIO_SAMPLE_VALUE)) {
        // The largest magnitude the secondary sample can be to avoid clipping (converted to Q30 fixed point)
        int32_t q30_secondary_sample_safe_max =
            static_cast<int32_t>(std::abs(MIN_AUDIO_SAMPLE_VALUE) - std::abs(primary_sample)) << 15;

        // Actual secondary sample value (Q15 number stored in an int32 for future division)
        int32_t absolute_secondary_sample_value = abs(secondary_sample);

        // Calculation to perform the Q15 division for secondary_sample_safe_max/secondary_sample_value
        // Reference: https://sestevenson.wordpress.com/2010/09/20/fixed-point-division-2/ (accessed August 15,
        // 2024)
        int16_t necessary_q15_factor =
            static_cast<int16_t>(q30_secondary_sample_safe_max / absolute_secondary_sample_value);
        // Take the minimum scaling factor (the smaller the factor, the more it needs to be scaled down)
        q15_scaling_factor = std::min(necessary_q15_factor, q15_scaling_factor);
      } else {
        // Store the combined samples in the output buffer. If we do not need to scale, then the samples are already
        // mixed.
        output_buffer[frames_index * output_channels + output_channel_index] = added_sample;
      }
    }
  }

  if (q15_scaling_factor < MAX_AUDIO_SAMPLE_VALUE) {
    // Need to scale to avoid clipping
    audio::scale_audio_samples(secondary_buffer, secondary_buffer, q15_scaling_factor,
                               frames_to_mix * secondary_channels);

    for (uint8_t output_channel_index = 0; output_channel_index < output_channels; ++output_channel_index) {
      const ssize_t secondary_channel_index = std::min(output_channel_index, max_secondary_channel_index);
      const ssize_t primary_channel_index = std::min(output_channel_index, max_primary_channel_index);
      const ssize_t output_channel_index_offset = output_channel_index * frames_to_mix;

      dsps_add_s16(secondary_buffer + secondary_channel_index, primary_buffer + primary_channel_index,
                   output_buffer + output_channel_index_offset, frames_to_mix, secondary_channels, primary_channels,
                   output_channels, 0);
    }
  }
}

void SpeakerMixer::audio_mixer_task(void *params) {
  SpeakerMixer *this_mixer = (SpeakerMixer *) params;

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_STARTING);

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer = audio::AudioSinkTransferBuffer::create(
      TRANSFER_BUFFER_DURATION_MS * this_mixer->audio_stream_info_.value().get_bytes_per_ms());

  if (output_transfer_buffer == nullptr) {
    xEventGroupSetBits(this_mixer->event_group_,
                       MixerEventGroupBits::STATE_STOPPED | MixerEventGroupBits::ERR_ESP_NO_MEM);

    this_mixer->task_created_ = false;
    vTaskDelete(nullptr);
  }

  output_transfer_buffer->set_sink(this_mixer->output_speaker_);

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_RUNNING);

  while (true) {
    std::shared_ptr<audio::AudioSourceTransferBuffer> primary_transfer_buffer =
        this_mixer->source_speakers_[0]->get_transfer_buffer().lock();
    std::shared_ptr<audio::AudioSourceTransferBuffer> secondary_transfer_buffer =
        this_mixer->source_speakers_[1]->get_transfer_buffer().lock();

    audio::AudioStreamInfo primary_stream_info = this_mixer->source_speakers_[0]->get_audio_stream_info();
    audio::AudioStreamInfo secondary_stream_info = this_mixer->source_speakers_[1]->get_audio_stream_info();

    uint32_t event_group_bits = xEventGroupGetBits(this_mixer->event_group_);
    if (event_group_bits & MixerEventGroupBits::COMMAND_STOP) {
      break;
    }

    output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(TASK_DELAY_MS));

    const uint32_t output_frames_free =
        output_transfer_buffer->free() / this_mixer->audio_stream_info_.value().get_bytes_per_frame();

    uint32_t primary_frames_available = 0;
    if (primary_transfer_buffer.use_count() > 0) {
      this_mixer->source_speakers_[0]->process_data_from_source(0);
      primary_frames_available = primary_transfer_buffer->available() / primary_stream_info.get_bytes_per_frame();
    }

    uint32_t secondary_frames_available = 0;
    if (secondary_transfer_buffer.use_count() > 0) {
      this_mixer->source_speakers_[1]->process_data_from_source(0);
      secondary_frames_available = secondary_transfer_buffer->available() / secondary_stream_info.get_bytes_per_frame();
    }

    // Restrict the number of frames available to the amount that the output transfer buffer can store
    primary_frames_available = std::min(primary_frames_available, output_frames_free);
    secondary_frames_available = std::min(secondary_frames_available, output_frames_free);

    if (secondary_frames_available + primary_frames_available > 0) {
      // Copies audio based on frames instead of bytes to avoid ever transferring half a sample or frame

      size_t bytes_written = 0;
      if ((secondary_frames_available > 0) && (primary_frames_available > 0)) {
        // Mix the audio
        uint32_t frames_to_transfer = std::min(secondary_frames_available, primary_frames_available);

        this_mixer->mix_audio_samples_without_clipping(
            reinterpret_cast<int16_t *>(primary_transfer_buffer->get_buffer_start()), primary_stream_info,
            reinterpret_cast<int16_t *>(secondary_transfer_buffer->get_buffer_start()), secondary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), frames_to_transfer);

        size_t primary_bytes_read = frames_to_transfer * primary_stream_info.get_bytes_per_frame();
        size_t secondary_bytes_read = frames_to_transfer * secondary_stream_info.get_bytes_per_frame();
        bytes_written = frames_to_transfer * this_mixer->audio_stream_info_.value().get_bytes_per_frame();

        primary_transfer_buffer->decrease_buffer_length(primary_bytes_read);
        secondary_transfer_buffer->decrease_buffer_length(secondary_bytes_read);
      } else if (primary_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames(
            reinterpret_cast<int16_t *>(primary_transfer_buffer->get_buffer_start()), primary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), primary_frames_available, bytes_read, bytes_written);

        primary_transfer_buffer->decrease_buffer_length(bytes_read);
      } else if (secondary_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames(
            reinterpret_cast<int16_t *>(secondary_transfer_buffer->get_buffer_start()), secondary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), secondary_frames_available, bytes_read, bytes_written);

        secondary_transfer_buffer->decrease_buffer_length(bytes_read);
      }

      output_transfer_buffer->increase_buffer_length(bytes_written);
    } else {
      // No audio data available in either buffer
      delay(TASK_DELAY_MS);
    }
  }

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_STOPPING);

  output_transfer_buffer.reset();

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_STOPPED);
  this_mixer->task_created_ = false;
  vTaskDelete(nullptr);
}

}  // namespace speaker_mixer
}  // namespace esphome

#endif
