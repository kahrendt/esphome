#ifdef USE_ESP32

#include "mixer_speaker.h"

#include "esphome/core/log.h"

#include <dsp.h>  // esp_audio_libs

namespace esphome {
namespace mixer_speaker {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const size_t TRANSFER_BUFFER_SIZE = 8192;

static const uint32_t TASK_STACK_SIZE = 3072;
static const size_t TASK_DELAY_MS = 25;

static const int16_t MAX_AUDIO_SAMPLE_VALUE = INT16_MAX;
static const int16_t MIN_AUDIO_SAMPLE_VALUE = INT16_MIN;

static const char *const TAG = "mixing_speaker";

enum MixerEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // stops the mixer task
  STATE_STARTING = (1 << 10),
  STATE_RUNNING = (1 << 11),
  STATE_STOPPING = (1 << 12),
  STATE_STOPPED = (1 << 13),
  ERR_ESP_NO_MEM = (1 << 19),
  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void InputSpeaker::loop() {
  if (this->state_ == speaker::STATE_RUNNING) {
    if (((millis() - this->last_seen_data_ms_) > this->timeout_ms_) && !this->has_buffered_data()) {
      this->state_ = speaker::STATE_STOPPED;
      this->stop_gracefully_ = false;
    }
    if (this->stop_gracefully_ && !this->has_buffered_data()) {
      this->state_ = speaker::STATE_STOPPED;
      this->stop_gracefully_ = false;
    }
  }
}

size_t InputSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  this->last_seen_data_ms_ = millis();
  if (this->is_stopped()) {
    this->start();
  }
  size_t bytes_written = 0;
  if (this->ring_buffer_.use_count() > 0) {
    bytes_written = this->ring_buffer_->write_without_replacement(data, length, ticks_to_wait);
  }
  return bytes_written;
}

void InputSpeaker::start() {
  const size_t ring_buffer_size = MIXER_INPUT_RING_BUFFER_DURATION_MS * this->audio_stream_info_.get_bytes_per_ms();
  this->ring_buffer_ = RingBuffer::create(ring_buffer_size);

  if ((this->ring_buffer_.use_count() == 0) || !this->allocate_buffer_(TRANSFER_BUFFER_SIZE)) {
    // Error state, wasn't able to allocate a buffer
  }

  if (this->parent_->start(this->audio_stream_info_) == ESP_OK) {
    this->state_ = speaker::STATE_RUNNING;
    this->stop_gracefully_ = false;
    this->status_clear_error();
  } else {
    this->state_ = speaker::STATE_STOPPED;
    this->status_set_error();
  }
}

void InputSpeaker::stop() {
  if (this->ring_buffer_.use_count() > 0) {
    this->ring_buffer_->reset();
  }

  this->state_ = speaker::STATE_STOPPED;
}

void InputSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->parent_->get_output_speaker()->set_mute_state(mute_state);
}

void InputSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->parent_->get_output_speaker()->set_volume(volume);
}

bool InputSpeaker::has_buffered_data() const {
  if (this->ring_buffer_.use_count() > 0) {
    return (this->ring_buffer_->available() > 0);
  }
  return false;
}

void InputSpeaker::set_ducking_reduction(uint8_t decibel_reduction, uint32_t duration) {
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
    if (total_ducking_steps > 0) {
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

size_t InputSpeaker::transfer_data_from_source(TickType_t ticks_to_wait) {
  // Shift data in buffer to start
  if (this->buffer_length_ > 0) {
    memmove(this->buffer_, this->data_start_, this->buffer_length_);
  }
  this->data_start_ = this->buffer_;

  uint8_t *data_end = this->get_buffer_end();

  size_t bytes_to_read = this->free();
  size_t bytes_read = 0;
  if (bytes_to_read > 0) {
    if (this->ring_buffer_.use_count() > 0) {
      bytes_read = this->ring_buffer_->read((void *) data_end, bytes_to_read, ticks_to_wait);
    }

    size_t samples_to_duck = bytes_read / this->audio_stream_info_.get_bytes_per_sample();

    if (samples_to_duck > 0) {
      int16_t *current_buffer = reinterpret_cast<int16_t *>(data_end);

      this->parent_->duck_samples(current_buffer, samples_to_duck, this->current_ducking_db_reduction_,
                                  this->ducking_transition_samples_remaining_, this->samples_per_ducking_step_,
                                  this->db_change_per_ducking_step_);
    }

    this->increase_buffer_length(bytes_read);
  }
  return bytes_read;
}

void MixerSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
}

esp_err_t MixerSpeaker::start(audio::AudioStreamInfo &stream_info) {
  ESP_LOGD(TAG, "Starting mixing speaker");
  if (!this->audio_stream_info_.has_value()) {
    if ((stream_info.channels > 2) || (stream_info.bits_per_sample != 16)) {
      // Audio streams with more than 2 channels or bits per sample not 16 bits are not supported
      return ESP_ERR_NOT_SUPPORTED;
    }

    this->audio_stream_info_ = stream_info;
    // The mixing speaker will always output 2 channels.
    this->audio_stream_info_.value().channels = 2;
    this->output_speaker_->set_audio_stream_info(this->audio_stream_info_.value());

    this->ring_buffer_size_ = MIXER_INPUT_RING_BUFFER_DURATION_MS * this->audio_stream_info_.value().get_bytes_per_ms();
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

void MixerSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & MixerEventGroupBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting Mixer");
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_STARTING);
  }
  if (event_group_bits & MixerEventGroupBits::ERR_ESP_NO_MEM) {
    this->status_set_error("Failed to allocate the mixer's internal buffer");
  }
  if (event_group_bits & MixerEventGroupBits::STATE_RUNNING) {
    ESP_LOGD(TAG, "Started Mixer");
    xEventGroupClearBits(this->event_group_, MixerEventGroupBits::STATE_RUNNING);
    this->status_clear_error();
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
}

void MixerSpeaker::audio_mixer_task(void *params) {
  MixerSpeaker *this_mixer = (MixerSpeaker *) params;

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_STARTING);

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(TRANSFER_BUFFER_SIZE);

  if (output_transfer_buffer == nullptr) {
    xEventGroupSetBits(this_mixer->event_group_,
                       MixerEventGroupBits::STATE_STOPPED | MixerEventGroupBits::ERR_ESP_NO_MEM);

    this_mixer->task_created_ = false;
    vTaskDelete(nullptr);
  }

  output_transfer_buffer->set_sink(this_mixer->output_speaker_);

  xEventGroupSetBits(this_mixer->event_group_, MixerEventGroupBits::STATE_RUNNING);

  while (true) {
    uint32_t event_group_bits = xEventGroupGetBits(this_mixer->event_group_);
    if (event_group_bits & MixerEventGroupBits::COMMAND_STOP) {
      break;
    }

    output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(TASK_DELAY_MS));

    const uint32_t output_frames_free =
        output_transfer_buffer->free() / this_mixer->audio_stream_info_.value().get_bytes_per_frame();

    audio::AudioStreamInfo primary_stream_info = this_mixer->primary_speaker_->get_audio_stream_info();
    this_mixer->primary_speaker_->transfer_data_from_source(0);
    uint32_t primary_frames_available =
        this_mixer->primary_speaker_->available() / primary_stream_info.get_bytes_per_frame();

    const size_t secondary_bytes_ducked = this_mixer->secondary_speaker_->available();
    audio::AudioStreamInfo secondary_stream_info = this_mixer->secondary_speaker_->get_audio_stream_info();
    this_mixer->secondary_speaker_->transfer_data_from_source(0);
    uint32_t secondary_frames_available =
        this_mixer->secondary_speaker_->available() / secondary_stream_info.get_bytes_per_frame();

    // Restrict the number of frames available to the amount that the output transfer buffer can store
    primary_frames_available = std::min(primary_frames_available, output_frames_free);
    secondary_frames_available = std::min(secondary_frames_available, output_frames_free);

    if (secondary_frames_available + primary_frames_available > 0) {
      // Copies audio based on frames instead of bytes to avoid ever transferring half a sample or frame

      size_t bytes_written = 0;
      if ((secondary_frames_available > 0) && (primary_frames_available > 0)) {
        // Mix the audio
        uint32_t frames_to_transfer = std::min(secondary_frames_available, primary_frames_available);

        this_mixer->mix_audio_samples_without_clipping_(
            reinterpret_cast<int16_t *>(this_mixer->primary_speaker_->get_buffer_start()), primary_stream_info,
            reinterpret_cast<int16_t *>(this_mixer->secondary_speaker_->get_buffer_start()), secondary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), frames_to_transfer);

        size_t primary_bytes_read = frames_to_transfer * primary_stream_info.get_bytes_per_frame();
        size_t secondary_bytes_read = frames_to_transfer * secondary_stream_info.get_bytes_per_frame();
        bytes_written = frames_to_transfer * this_mixer->audio_stream_info_.value().get_bytes_per_frame();

        this_mixer->primary_speaker_->decrease_buffer_length(primary_bytes_read);
        this_mixer->secondary_speaker_->decrease_buffer_length(secondary_bytes_read);
      } else if (primary_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames_(
            reinterpret_cast<int16_t *>(this_mixer->primary_speaker_->get_buffer_start()), primary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), primary_frames_available, bytes_read, bytes_written);

        this_mixer->primary_speaker_->decrease_buffer_length(bytes_read);
      } else if (secondary_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames_(
            reinterpret_cast<int16_t *>(this_mixer->secondary_speaker_->get_buffer_start()), secondary_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), secondary_frames_available, bytes_read, bytes_written);

        this_mixer->secondary_speaker_->decrease_buffer_length(bytes_read);
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

void MixerSpeaker::duck_samples(int16_t *input_buffer, uint32_t input_samples_to_duck,
                                int8_t &current_ducking_db_reduction, size_t &ducking_transition_samples_remaining,
                                size_t samples_per_ducking_step, int8_t db_change_per_ducking_step) {
  if (ducking_transition_samples_remaining > 0) {
    // Ducking level is still transitioning

    // Take the ceiling of secondary_samples_to_duck/samples_per_ducking_step
    size_t ducking_steps_in_batch =
        input_samples_to_duck / samples_per_ducking_step + (input_samples_to_duck % samples_per_ducking_step != 0);

    for (size_t i = 0; i < ducking_steps_in_batch; ++i) {
      size_t samples_left_in_step = ducking_transition_samples_remaining % samples_per_ducking_step;

      if (samples_left_in_step == 0) {
        samples_left_in_step = samples_per_ducking_step;
      }

      size_t samples_to_duck = std::min(input_samples_to_duck, samples_left_in_step);
      samples_to_duck = std::min(samples_to_duck, ducking_transition_samples_remaining);

      // Ensure we only point to valid index in the Q15 scaling factor table
      uint8_t safe_db_reduction_index =
          clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
      int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

      this->scale_audio_samples_(input_buffer, input_buffer, q15_scale_factor, samples_to_duck);

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
        clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
    int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

    this->scale_audio_samples_(input_buffer, input_buffer, q15_scale_factor, input_samples_to_duck);
  }
}

void MixerSpeaker::copy_frames_(int16_t *input_buffer, audio::AudioStreamInfo input_stream_info, int16_t *output_buffer,
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

void MixerSpeaker::mix_audio_samples_without_clipping_(
    int16_t *primary_buffer, audio::AudioStreamInfo primary_stream_info, int16_t *secondary_buffer,
    audio::AudioStreamInfo secondary_stream_info, int16_t *output_buffer, audio::AudioStreamInfo output_stream_info,
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
    this->scale_audio_samples_(secondary_buffer, secondary_buffer, q15_scaling_factor,
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

void MixerSpeaker::scale_audio_samples_(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                                        size_t samples_to_scale) {
  // Note the assembly dsps_mulc function has audio glitches if the input and output buffers are the same.
  for (int i = 0; i < samples_to_scale; i++) {
    int32_t acc = (int32_t) audio_samples[i] * (int32_t) scale_factor;
    output_buffer[i] = (int16_t) (acc >> 15);
  }
}

}  // namespace mixer_speaker
}  // namespace esphome

#endif
