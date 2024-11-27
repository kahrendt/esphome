#ifdef USE_ESP32

#include "mixer_speaker.h"

#include <dsp.h>  // esp_audio_libs

namespace esphome {
namespace mixer_speaker {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const size_t TRANSFER_BUFFER_SIZE = 4096;

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

void MixerSpeaker::start(audio::AudioStreamInfo &stream_info) {
  if (!this->audio_stream_info_.has_value()) {
    this->audio_stream_info_ = stream_info;
    this->output_speaker_->set_audio_stream_info(stream_info);
  } else {
    if (stream_info != this->audio_stream_info_.value()) {
      printf("mismatching audio stream info, can't play");
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

    media_transfer_buffer->transfer_data_from_source(0);
    announcement_transfer_buffer->transfer_data_from_source(0);

    audio::AudioStreamInfo announcement_stream_info = this_mixer->announcement_speaker_->get_audio_stream_info();
    audio::AudioStreamInfo media_stream_info = this_mixer->media_speaker_->get_audio_stream_info();

    const size_t media_available = std::min(media_transfer_buffer->available(), output_transfer_buffer->free());
    // size_t media_frames_available = media_available / sizeof(int16_t) / 2;
    size_t media_frames_available = media_available / media_stream_info.get_bytes_per_frame();

    const size_t announcement_available =
        std::min(announcement_transfer_buffer->available(),
                 output_transfer_buffer->free() / 2);  // assumes we'll double the samples to convert mono to stereo
    size_t announcement_frames_available = announcement_available / announcement_stream_info.get_bytes_per_frame();

    if (media_frames_available + announcement_frames_available > 0) {
      if (media_frames_available > 0) {
        // Duck here
        size_t frames_read = media_frames_available;

        if (announcement_frames_available > 0) {
          // We'll mix in announcement audio as well, so that limits the number of samples we proccess
          frames_read = std::min(frames_read, announcement_frames_available);
        }
        size_t media_samples_read = frames_read * 2;

        // There may be more than one step worth of samples to duck in the buffers, so manage positions
        int16_t *current_media_buffer = reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start());

        if (ducking_transition_samples_remaining > 0) {
          // Ducking level is still transitioning

          size_t current_ducking_transition_samples_remaining = ducking_transition_samples_remaining;

          // Take the ceiling of media_samples_read/samples_per_ducking_step
          size_t ducking_steps_in_batch =
              media_samples_read / samples_per_ducking_step + (media_samples_read % samples_per_ducking_step != 0);

          for (size_t i = 0; i < ducking_steps_in_batch; ++i) {
            size_t samples_left_in_step = current_ducking_transition_samples_remaining % samples_per_ducking_step;

            if (samples_left_in_step == 0) {
              samples_left_in_step = samples_per_ducking_step;
            }

            size_t samples_to_duck = std::min(media_samples_read, samples_left_in_step);
            samples_to_duck = std::min(samples_to_duck, current_ducking_transition_samples_remaining);

            // Ensure we only point to valid index in the Q15 scaling factor table
            uint8_t safe_db_reduction_index =
                clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
            int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

            this_mixer->scale_audio_samples_(current_media_buffer, current_media_buffer, q15_scale_factor,
                                             samples_to_duck);

            if (samples_left_in_step - samples_to_duck == 0) {
              // After scaling the current samples, we are ready to transition to the next step
              current_ducking_db_reduction += db_change_per_ducking_step;
            }

            current_media_buffer += samples_to_duck;
            current_ducking_transition_samples_remaining -= samples_to_duck;
            media_samples_read -= samples_to_duck;
          }
        }

        if ((current_ducking_db_reduction > 0) && (media_samples_read > 0)) {
          // We still need to apply ducking, but we are not in the middle of a transition step

          uint8_t safe_db_reduction_index =
              clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
          int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

          this_mixer->scale_audio_samples_(current_media_buffer, current_media_buffer, q15_scale_factor,
                                           media_samples_read);
        }
      }

      // Copy based on samples written instead of bytes to avoid ever transferring half a sample
      // size_t samples_written = 0;
      size_t frames_written = 0;
      if ((media_frames_available > 0) && (announcement_frames_available > 0)) {
        frames_written = std::min(media_frames_available, announcement_frames_available);

        this_mixer->mix_audio_samples_without_clipping_(
            reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start()),
            reinterpret_cast<int16_t *>(announcement_transfer_buffer->get_buffer_start()),
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), frames_written);

        media_transfer_buffer->decrease_buffer_length(frames_written * media_stream_info.get_bytes_per_frame());
        announcement_transfer_buffer->decrease_buffer_length(frames_written *
                                                             announcement_stream_info.get_bytes_per_frame());
      } else if (media_frames_available > 0) {
        frames_written = media_frames_available;
        memcpy(output_transfer_buffer->get_buffer_end(), media_transfer_buffer->get_buffer_start(),
               frames_written * media_stream_info.get_bytes_per_frame());
        media_transfer_buffer->decrease_buffer_length(frames_written * media_stream_info.get_bytes_per_frame());
      } else if (announcement_frames_available > 0) {
        frames_written = announcement_frames_available;
        if (announcement_stream_info.channels == 1) {
          int16_t *announcement_data_to_read =
              reinterpret_cast<int16_t *>(announcement_transfer_buffer->get_buffer_start());
          int16_t *output_buffer_data_to_write = reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end());

          for (size_t i = 0; i < frames_written; ++i) {
            output_buffer_data_to_write[2 * i] = announcement_data_to_read[i];
            output_buffer_data_to_write[2 * i + 1] = announcement_data_to_read[i];
          }

        } else {
          memcpy(output_transfer_buffer->get_buffer_end(), announcement_transfer_buffer->get_buffer_start(),
                 frames_written * announcement_stream_info.get_bytes_per_frame());
        }
        announcement_transfer_buffer->decrease_buffer_length(frames_written *
                                                             announcement_stream_info.get_bytes_per_frame());
      }

      output_transfer_buffer->increase_buffer_length(frames_written * sizeof(int16_t) * 2);
      if (ducking_transition_samples_remaining > 0) {
        // Advance ducking transition samples whenever any audio is sent
        ducking_transition_samples_remaining -=
            std::min(frames_written * media_stream_info.get_bytes_per_frame(), ducking_transition_samples_remaining);
      }
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

void MixerSpeaker::mix_audio_samples_without_clipping_(int16_t *media_buffer, int16_t *announcement_buffer,
                                                       int16_t *output_buffer, size_t frames_to_mix) {
  // We first test adding the two clips samples together and check for any clipping
  // We want the announcement volume to be consistent, regardless if media is playing or not
  // If there is clipping, we determine what factor we need to multiply that media sample by to avoid it
  // We take the smallest factor necessary for all the samples so the media volume is consistent on this batch
  // of samples
  // Note: This may not be the best approach. Adding 2 audio samples together makes both sound louder, even if
  // we are not clipping. As a result, the mixed announcement will sound louder (by around 3dB if the audio
  // streams are independent?) than if it were by itself.

  int16_t q15_scaling_factor = MAX_AUDIO_SAMPLE_VALUE;

  for (size_t i = 0; i < frames_to_mix * 2; ++i) {
    int32_t added_sample = static_cast<int32_t>(media_buffer[i]) + static_cast<int32_t>(announcement_buffer[i / 2]);

    if ((added_sample > MAX_AUDIO_SAMPLE_VALUE) || (added_sample < MIN_AUDIO_SAMPLE_VALUE)) {
      // The largest magnitude the media sample can be to avoid clipping (converted to Q30 fixed point)
      int32_t q30_media_sample_safe_max =
          static_cast<int32_t>(std::abs(MIN_AUDIO_SAMPLE_VALUE) - std::abs(announcement_buffer[i / 2])) << 15;

      // Actual media sample value (Q15 number stored in an int32 for future division)
      int32_t media_sample_value = abs(media_buffer[i]);

      // Calculation to perform the Q15 division for media_sample_safe_max/media_sample_value
      // Reference: https://sestevenson.wordpress.com/2010/09/20/fixed-point-division-2/ (accessed August 15,
      // 2024)
      int16_t necessary_q15_factor = static_cast<int16_t>(q30_media_sample_safe_max / media_sample_value);
      // Take the minimum scaling factor (the smaller the factor, the more it needs to be scaled down)
      q15_scaling_factor = std::min(necessary_q15_factor, q15_scaling_factor);
    } else {
      // Store the combined samples in the output buffer. If we do not need to scale, then the samples are already
      // mixed.
      output_buffer[i] = added_sample;
    }
  }

  if (q15_scaling_factor < MAX_AUDIO_SAMPLE_VALUE) {
    // Need to scale to avoid clipping
    this->scale_audio_samples_(media_buffer, media_buffer, q15_scaling_factor, frames_to_mix * 2);

    // Mix both stream by adding them together with no bitshift
    // The dsps_add functions have the following inputs:
    // (buffer 1, buffer 2, output buffer, number of samples, buffer 1 step, buffer 2 step, output buffer step,
    // bitshift)
    if (this->announcement_channel_divisor_ == 2) {
      dsps_add_s16(media_buffer, announcement_buffer, output_buffer, frames_to_mix, 2, 1, 2, 0);
      dsps_add_s16(media_buffer + 1, announcement_buffer, output_buffer + 1, frames_to_mix, 2, 1, 2, 0);
    } else {
      dsps_add_s16(media_buffer, announcement_buffer, output_buffer, frames_to_mix * 2, 1, 1, 1, 0);
    }
  }
}

void MixerSpeaker::scale_audio_samples_(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                                        size_t samples_to_scale) {
  // Note the assembly dsps_mulc function has glitches if the input and output buffers are the same.
  for (int i = 0; i < samples_to_scale; i++) {
    int32_t acc = (int32_t) audio_samples[i] * (int32_t) scale_factor;
    output_buffer[i] = (int16_t) (acc >> 15);
  }
}

}  // namespace mixer_speaker
}  // namespace esphome

#endif
