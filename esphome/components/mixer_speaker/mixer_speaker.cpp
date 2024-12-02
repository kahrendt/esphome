#ifdef USE_ESP32

#include "mixer_speaker.h"

#include <dsp.h>  // esp_audio_libs

namespace esphome {
namespace mixer_speaker {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const size_t TRANSFER_BUFFER_SIZE = 8192;

static const size_t QUEUE_COUNT = 20;

static const uint32_t TASK_STACK_SIZE = 3072;
static const size_t TASK_DELAY_MS = 25;

static const int16_t MAX_AUDIO_SAMPLE_VALUE = INT16_MAX;
static const int16_t MIN_AUDIO_SAMPLE_VALUE = INT16_MIN;

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
  this->parent_->start(this->audio_stream_info_);
  this->state_ = speaker::STATE_RUNNING;
  this->stop_gracefully_ = false;
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

void MixerSpeaker::setup() {
  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  if ((this->event_queue_ == nullptr) || (this->command_queue_ == nullptr)) {
    this->mark_failed();
  }
}

void MixerSpeaker::set_ducking_reduction(uint8_t decibel_reduction, uint32_t duration) {
  if (this->command_queue_ != nullptr) {
    CommandEvent command_event;
    command_event.command = CommandEventType::DUCK;
    command_event.decibel_reduction = decibel_reduction;

    // Convert the duration in seconds to number of samples, accounting for the sample rate and number of channels
    command_event.transition_samples = duration * this->media_speaker_->get_audio_stream_info().get_samples_per_ms();
    this->send_command_(&command_event);
  }
}

void MixerSpeaker::start(audio::AudioStreamInfo &stream_info) {
  if (!this->audio_stream_info_.has_value()) {
    if (stream_info.channels > 2) {
      printf("unsupported number of channels\n");
    }
    if (stream_info.bits_per_sample != 16) {
      printf("unsupported bits per sample\n");
    }

    this->audio_stream_info_ = stream_info;
    // The mixing speaker will always output 2 channels.
    this->audio_stream_info_.value().channels = 2;
    this->output_speaker_->set_audio_stream_info(this->audio_stream_info_.value());
  } else {
    if (stream_info.sample_rate != this->audio_stream_info_.value().sample_rate) {
      printf("mismatching sample rates, can't mix the two streams\n");
    } else if (stream_info.bits_per_sample != this->audio_stream_info_.value().bits_per_sample) {
      printf("mismatching bits per sample, can't mix the two streams\n");
    }
  }

  this->ring_buffer_size_ = MIXER_INPUT_RING_BUFFER_DURATION_MS * stream_info.get_bytes_per_ms();

  if (this->task_handle_ == nullptr) {
    xTaskCreate(this->audio_mixer_task, "mixer", TASK_STACK_SIZE, (void *) this, MIXER_TASK_PRIORITY,
                &this->task_handle_);
  }

  // TODO: Test that the task actually started

  this->set_retry(50, 2, [this](const uint8_t remaining_setup_attempts) {
    if ((this->announcement_ring_buffer_.use_count() == 0) || (this->media_ring_buffer_.use_count() == 0)) {
      if (remaining_setup_attempts == 0) {
        this->status_set_error("Error starting the audio pipeline since the mixer hasn't finished allocating buffers");
      }
      return RetryResult::RETRY;
    }

    this->announcement_speaker_->set_sink(this->announcement_ring_buffer_);
    this->media_speaker_->set_sink(this->media_ring_buffer_);

    return RetryResult::DONE;
  });
}

void MixerSpeaker::audio_mixer_task(void *params) {
  MixerSpeaker *this_mixer = (MixerSpeaker *) params;

  TaskEvent event;
  CommandEvent command_event;

  esp_err_t err = ESP_OK;

  std::unique_ptr<audio::AudioSourceTransferBuffer> media_transfer_buffer =
      audio::AudioSourceTransferBuffer::create(TRANSFER_BUFFER_SIZE);

  {  // After this block temp_media_ring_buffer will fall out of scope and release ownership
    std::shared_ptr<RingBuffer> temp_media_ring_buffer;
    if (this_mixer->media_ring_buffer_.use_count() == 0) {
      temp_media_ring_buffer = RingBuffer::create(this_mixer->ring_buffer_size_);
      this_mixer->media_ring_buffer_ = temp_media_ring_buffer;
    }
    if (this_mixer->media_ring_buffer_.use_count() == 0) {
      err = ESP_ERR_NO_MEM;
    } else {
      media_transfer_buffer->set_source(temp_media_ring_buffer);
    }
  }

  std::unique_ptr<audio::AudioSourceTransferBuffer> announcement_transfer_buffer =
      audio::AudioSourceTransferBuffer::create(TRANSFER_BUFFER_SIZE);

  {  // After this block temp_announncement_ring_buffer will fall out of scope and release ownership
    std::shared_ptr<RingBuffer> temp_announcement_ring_buffer;
    if (this_mixer->announcement_ring_buffer_.use_count() == 0) {
      temp_announcement_ring_buffer = RingBuffer::create(this_mixer->ring_buffer_size_);
      this_mixer->announcement_ring_buffer_ = temp_announcement_ring_buffer;
    }
    if (this_mixer->announcement_ring_buffer_.use_count() == 0) {
      err = ESP_ERR_NO_MEM;
    } else {
      announcement_transfer_buffer->set_source(temp_announcement_ring_buffer);
    }
  }

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(TRANSFER_BUFFER_SIZE);
  output_transfer_buffer->set_sink(this_mixer->output_speaker_);

  if ((media_transfer_buffer == nullptr) || (announcement_transfer_buffer == nullptr) ||
      (output_transfer_buffer == nullptr) || (err == ESP_ERR_NO_MEM)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(TASK_DELAY_MS);
    }
  }

  // Parameters to control the ducking dB reduction and its transitions
  // There is a built in negative sign; e.g., reducing by 5 dB is changing the gain by -5 dB
  int8_t target_ducking_db_reduction = 0;
  int8_t current_ducking_db_reduction = 0;

  // Each step represents a change in 1 dB. Positive 1 means the dB reduction is increasing. Negative 1 means the dB
  // reduction is decreasing.
  int8_t db_change_per_ducking_step = 1;

  size_t ducking_transition_samples_remaining = 0;
  size_t samples_per_ducking_step = 0;

  event.type = EventType::STARTED;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    if (xQueueReceive(this_mixer->command_queue_, &command_event, 0) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::DUCK) {
        if (target_ducking_db_reduction != command_event.decibel_reduction) {
          current_ducking_db_reduction = target_ducking_db_reduction;

          target_ducking_db_reduction = command_event.decibel_reduction;

          uint8_t total_ducking_steps = 0;
          if (target_ducking_db_reduction > current_ducking_db_reduction) {
            // The dB reduction level is increasing (which results in quieter audio)
            total_ducking_steps = target_ducking_db_reduction - current_ducking_db_reduction - 1;
            db_change_per_ducking_step = 1;
          } else {
            // The dB reduction level is decreasing (which results in louder audio)
            total_ducking_steps = current_ducking_db_reduction - target_ducking_db_reduction - 1;
            db_change_per_ducking_step = -1;
          }
          if (total_ducking_steps > 0) {
            ducking_transition_samples_remaining = command_event.transition_samples;

            samples_per_ducking_step = ducking_transition_samples_remaining / total_ducking_steps;
            ducking_transition_samples_remaining =
                samples_per_ducking_step * total_ducking_steps;  // Adjust for integer division rounding

            current_ducking_db_reduction += db_change_per_ducking_step;
          } else {
            ducking_transition_samples_remaining = 0;
            current_ducking_db_reduction = target_ducking_db_reduction;
          }
        }
      }
    }

    output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(TASK_DELAY_MS));

    const uint32_t output_frames_free =
        output_transfer_buffer->free() / this_mixer->audio_stream_info_.value().get_bytes_per_frame();

    audio::AudioStreamInfo announcement_stream_info = this_mixer->announcement_speaker_->get_audio_stream_info();
    announcement_transfer_buffer->transfer_data_from_source(0);
    uint32_t announcement_frames_available =
        announcement_transfer_buffer->available() / announcement_stream_info.get_bytes_per_frame();

    const size_t media_bytes_ducked = media_transfer_buffer->available();
    audio::AudioStreamInfo media_stream_info = this_mixer->media_speaker_->get_audio_stream_info();
    media_transfer_buffer->transfer_data_from_source(0);
    uint32_t media_frames_available = media_transfer_buffer->available() / media_stream_info.get_bytes_per_frame();
    size_t media_samples_to_duck =
        (media_transfer_buffer->available() - media_bytes_ducked) / media_stream_info.get_bytes_per_sample();

    if (media_samples_to_duck > 0) {
      int16_t *current_media_buffer =
          reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start() + media_bytes_ducked);

      this_mixer->duck_samples_(current_media_buffer, media_samples_to_duck, current_ducking_db_reduction,
                                ducking_transition_samples_remaining, samples_per_ducking_step,
                                db_change_per_ducking_step);
    }

    // Restrict the number of frames available to the amount that the output transfer buffer can store
    announcement_frames_available = std::min(announcement_frames_available, output_frames_free);
    media_frames_available = std::min(media_frames_available, output_frames_free);

    if (media_frames_available + announcement_frames_available > 0) {
      // Copies audio based on frames instead of bytes to avoid ever transferring half a sample or frame

      size_t bytes_written = 0;
      if ((media_frames_available > 0) && (announcement_frames_available > 0)) {
        // Mix the audio
        uint32_t frames_to_transfer = std::min(media_frames_available, announcement_frames_available);

        this_mixer->mix_audio_samples_without_clipping_(
            reinterpret_cast<int16_t *>(announcement_transfer_buffer->get_buffer_start()), announcement_stream_info,
            reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start()), media_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), frames_to_transfer);

        size_t announcement_bytes_read = frames_to_transfer * announcement_stream_info.get_bytes_per_frame();
        size_t media_bytes_read = frames_to_transfer * media_stream_info.get_bytes_per_frame();
        bytes_written = frames_to_transfer * this_mixer->audio_stream_info_.value().get_bytes_per_frame();

        announcement_transfer_buffer->decrease_buffer_length(announcement_bytes_read);
        media_transfer_buffer->decrease_buffer_length(media_bytes_read);
      } else if (announcement_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames_(
            reinterpret_cast<int16_t *>(announcement_transfer_buffer->get_buffer_start()), announcement_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), announcement_frames_available, bytes_read, bytes_written);

        announcement_transfer_buffer->decrease_buffer_length(bytes_read);
      } else if (media_frames_available > 0) {
        size_t bytes_read = 0;
        this_mixer->copy_frames_(
            reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start()), media_stream_info,
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()),
            this_mixer->audio_stream_info_.value(), media_frames_available, bytes_read, bytes_written);

        media_transfer_buffer->decrease_buffer_length(bytes_read);
      }

      output_transfer_buffer->increase_buffer_length(bytes_written);
    } else {
      // No audio data available in either buffer
      delay(TASK_DELAY_MS);
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  media_transfer_buffer.reset();
  announcement_transfer_buffer.reset();
  output_transfer_buffer.reset();

  event.type = EventType::STOPPED;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(TASK_DELAY_MS);
  }
}

BaseType_t MixerSpeaker::read_event_(TaskEvent *event, TickType_t ticks_to_wait) {
  if (this->event_queue_ != nullptr) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }
  return pdFALSE;
}

BaseType_t MixerSpeaker::send_command_(CommandEvent *command, TickType_t ticks_to_wait) {
  if (this->command_queue_ != nullptr) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }
  return pdFALSE;
}

void MixerSpeaker::duck_samples_(int16_t *input_buffer, uint32_t media_samples_to_duck,
                                 int8_t &current_ducking_db_reduction, size_t &ducking_transition_samples_remaining,
                                 size_t samples_per_ducking_step, int8_t db_change_per_ducking_step) {
  if (ducking_transition_samples_remaining > 0) {
    // Ducking level is still transitioning

    // Take the ceiling of media_samples_to_duck/samples_per_ducking_step
    size_t ducking_steps_in_batch =
        media_samples_to_duck / samples_per_ducking_step + (media_samples_to_duck % samples_per_ducking_step != 0);

    for (size_t i = 0; i < ducking_steps_in_batch; ++i) {
      size_t samples_left_in_step = ducking_transition_samples_remaining % samples_per_ducking_step;

      if (samples_left_in_step == 0) {
        samples_left_in_step = samples_per_ducking_step;
      }

      size_t samples_to_duck = std::min(media_samples_to_duck, samples_left_in_step);
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
      media_samples_to_duck -= samples_to_duck;
    }
  }

  if ((current_ducking_db_reduction > 0) && (media_samples_to_duck > 0)) {
    // We still need to apply ducking, but we are not in the middle of a transition step

    uint8_t safe_db_reduction_index =
        clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
    int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

    this->scale_audio_samples_(input_buffer, input_buffer, q15_scale_factor, media_samples_to_duck);
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
  // We want the announcement volume to be consistent, regardless if media is playing or not
  // If there is clipping, we determine what factor we need to multiply that media sample by to avoid it
  // We take the smallest factor necessary for all the samples so the media volume is consistent on this batch
  // of samples
  // Note: This may not be the best approach. Adding 2 audio samples together makes both sound louder, even if
  // we are not clipping. As a result, the mixed announcement will sound louder (by around 3dB if the audio
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
        // The largest magnitude the media sample can be to avoid clipping (converted to Q30 fixed point)
        int32_t q30_secondary_sample_safe_max =
            static_cast<int32_t>(std::abs(MIN_AUDIO_SAMPLE_VALUE) - std::abs(primary_sample)) << 15;

        // Actual media sample value (Q15 number stored in an int32 for future division)
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
