#include "audio_mixer.h"

#include <dsp.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace speaker {

static const size_t QUEUE_COUNT = 20;

static const uint32_t TASK_STACK_SIZE = 3072;
static const size_t TASK_DELAY_MS = 25;

static const int16_t MAX_AUDIO_SAMPLE_VALUE = INT16_MAX;
static const int16_t MIN_AUDIO_SAMPLE_VALUE = INT16_MIN;

std::unique_ptr<AudioMixer> AudioMixer::create(size_t ring_buffer_size, size_t transfer_buffer_size) {
  std::unique_ptr<AudioMixer> mixer = make_unique<AudioMixer>();

  mixer->ring_buffer_size_ = ring_buffer_size;
  mixer->transfer_buffer_size_ = transfer_buffer_size;

  esp_err_t err = ESP_OK;
  err = mixer->allocate_buffers_();

  if (err != ESP_OK) {
    return nullptr;
  }

  return mixer;
}

esp_err_t AudioMixer::start(Speaker *speaker, const std::string &task_name, UBaseType_t priority) {
  esp_err_t err = this->allocate_buffers_();

  if (err != ESP_OK) {
    return err;
  }

  if (this->task_handle_ == nullptr) {
    xTaskCreate(AudioMixer::audio_mixer_task, task_name.c_str(), TASK_STACK_SIZE, (void *) this, priority,
                &this->task_handle_);
  }

  if (this->task_handle_ == nullptr) {
    return ESP_FAIL;
  }

  this->speaker_ = speaker;

  return ESP_OK;
}

void AudioMixer::stop() {
  vTaskDelete(this->task_handle_);
  this->task_handle_ = nullptr;

  xQueueReset(this->event_queue_);
  xQueueReset(this->command_queue_);
}

void AudioMixer::suspend_task() {
  if (this->task_handle_ != nullptr) {
    vTaskSuspend(this->task_handle_);
  }
}

void AudioMixer::resume_task() {
  if (this->task_handle_ != nullptr) {
    vTaskResume(task_handle_);
  }
}

void AudioMixer::audio_mixer_task(void *params) {
  AudioMixer *this_mixer = (AudioMixer *) params;

  TaskEvent event;
  CommandEvent command_event;

  std::unique_ptr<audio::AudioSourceTransferBuffer> media_transfer_buffer =
      audio::AudioSourceTransferBuffer::create(this_mixer->transfer_buffer_size_);

  {  // After this block temp_media_ring_buffer will fall out of scope and release ownership
    std::shared_ptr<RingBuffer> temp_media_ring_buffer;
    if (this_mixer->media_ring_buffer_.use_count() == 0) {
      temp_media_ring_buffer = std::move(RingBuffer::create(this_mixer->ring_buffer_size_));
      this_mixer->media_ring_buffer_ = temp_media_ring_buffer;
    }
    if (this_mixer->media_ring_buffer_.use_count() == 0) {
      // TODO: Handle no allocation
    } else {
      media_transfer_buffer->set_source(temp_media_ring_buffer);
    }
  }

  std::unique_ptr<audio::AudioSourceTransferBuffer> announcement_transfer_buffer =
      audio::AudioSourceTransferBuffer::create(this_mixer->transfer_buffer_size_);

  {  // After this block temp_announncement_ring_buffer will fall out of scope and release ownership
    std::shared_ptr<RingBuffer> temp_announcement_ring_buffer;
    if (this_mixer->announcement_ring_buffer_.use_count() == 0) {
      temp_announcement_ring_buffer = std::move(RingBuffer::create(this_mixer->ring_buffer_size_));
      this_mixer->announcement_ring_buffer_ = temp_announcement_ring_buffer;
    }
    if (this_mixer->announcement_ring_buffer_.use_count() == 0) {
      // TODO: Handle no allocation
    } else {
      announcement_transfer_buffer->set_source(temp_announcement_ring_buffer);
    }
  }

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(this_mixer->transfer_buffer_size_);
  output_transfer_buffer->set_sink(this_mixer->speaker_);

  if ((media_transfer_buffer == nullptr) || (announcement_transfer_buffer == nullptr) ||
      (output_transfer_buffer == nullptr)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(TASK_DELAY_MS);
    }

    return;
  }

  // Handles media stream pausing
  bool transfer_media = true;

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
      } else if (command_event.command == CommandEventType::PAUSE_MEDIA) {
        transfer_media = false;
      } else if (command_event.command == CommandEventType::RESUME_MEDIA) {
        transfer_media = true;
        // printf("media_ring_buffer use count %ld\n", this_mixer->media_ring_buffer_.use_count());
        // } else if (command_event.command == CommandEventType::CLEAR_MEDIA) {
        //   this_mixer->media_ring_buffer_->reset();
        // } else if (command_event.command == CommandEventType::CLEAR_ANNOUNCEMENT) {
        // this_mixer->announcement_ring_buffer_->reset();
      }
    }

    if ((this_mixer->media_ring_buffer_.use_count() == 1) && (media_transfer_buffer->has_buffered_data())) {
      // Autoclear the data in the media ring buffer if the audio source no longer owns it
      //  - This ensures that if a new pipeline starts feeding the mixer while paused, it won't play the old audio
      media_transfer_buffer->clear_buffered_data();
    }
    if ((this_mixer->announcement_ring_buffer_.use_count() == 1) &&
        (announcement_transfer_buffer->has_buffered_data())) {
      // Autoclear the data in the announcement ring buffer if the audio source no longer owns it
      announcement_transfer_buffer->clear_buffered_data();
    }

    output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(TASK_DELAY_MS));

    media_transfer_buffer->transfer_data_from_source(0);
    announcement_transfer_buffer->transfer_data_from_source(0);

    size_t media_available =
        std::min(media_transfer_buffer->available() * transfer_media, output_transfer_buffer->free());
    size_t announcement_available = std::min(announcement_transfer_buffer->available(), output_transfer_buffer->free());

    if (media_available + announcement_available > 0) {
      if (media_available > 0) {
        // Duck here
        size_t samples_read = media_available / sizeof(int16_t);
        if (announcement_available > 0) {
          // We'll mix in announcement audio as well, so that limits the number of samples we proccess
          samples_read = std::min(samples_read, announcement_available / sizeof(int16_t));
        }

        // There may be more than one step worth of samples to duck in the buffers, so manage positions
        int16_t *current_media_buffer = reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start());

        if (ducking_transition_samples_remaining > 0) {
          // Ducking level is still transitioning

          size_t current_ducking_transition_samples_remaining = ducking_transition_samples_remaining;

          // Take the ceiling of samples_read/samples_per_ducking_step
          size_t ducking_steps_in_batch =
              samples_read / samples_per_ducking_step + (samples_read % samples_per_ducking_step != 0);

          for (size_t i = 0; i < ducking_steps_in_batch; ++i) {
            size_t samples_left_in_step = current_ducking_transition_samples_remaining % samples_per_ducking_step;

            if (samples_left_in_step == 0) {
              samples_left_in_step = samples_per_ducking_step;
            }

            size_t samples_to_duck = std::min(samples_read, samples_left_in_step);
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
            samples_read -= samples_to_duck;
          }
        }

        if ((current_ducking_db_reduction > 0) && (samples_read > 0)) {
          // We still need to apply ducking, but we are not in the middle of a transition step

          uint8_t safe_db_reduction_index =
              clamp<uint8_t>(current_ducking_db_reduction, 0, DECIBEL_REDUCTION_TABLE.size() - 1);
          int16_t q15_scale_factor = DECIBEL_REDUCTION_TABLE[safe_db_reduction_index];

          this_mixer->scale_audio_samples_(current_media_buffer, current_media_buffer, q15_scale_factor, samples_read);
        }
      }

      // Copy based on samples written instead of bytes to avoid ever transferring half a sample
      size_t samples_written = 0;
      if ((media_available > 0) && (announcement_available > 0)) {
        samples_written = std::min(media_available / sizeof(int16_t), announcement_available / sizeof(int16_t));
        this_mixer->mix_audio_samples_without_clipping_(
            reinterpret_cast<int16_t *>(media_transfer_buffer->get_buffer_start()),
            reinterpret_cast<int16_t *>(announcement_transfer_buffer->get_buffer_start()),
            reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), samples_written);

        media_transfer_buffer->decrease_buffer_length(samples_written * sizeof(int16_t));
        announcement_transfer_buffer->decrease_buffer_length(samples_written * sizeof(int16_t));
      } else if (media_available > 0) {
        samples_written = media_available / sizeof(int16_t);
        memcpy(output_transfer_buffer->get_buffer_end(), media_transfer_buffer->get_buffer_start(),
               samples_written * sizeof(int16_t));
        media_transfer_buffer->decrease_buffer_length(samples_written * sizeof(int16_t));
      } else if (announcement_available > 0) {
        samples_written = announcement_available / sizeof(int16_t);
        memcpy(output_transfer_buffer->get_buffer_end(), announcement_transfer_buffer->get_buffer_start(),
               samples_written * sizeof(int16_t));
        announcement_transfer_buffer->decrease_buffer_length(samples_written * sizeof(int16_t));
      }

      output_transfer_buffer->increase_buffer_length(samples_written * sizeof(int16_t));
      if (ducking_transition_samples_remaining > 0) {
        // Advance ducking transition samples whenever any audio is sent
        ducking_transition_samples_remaining -= std::min(samples_written, ducking_transition_samples_remaining);
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

esp_err_t AudioMixer::allocate_buffers_() {
  if (this->event_queue_ == nullptr)
    this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  if (this->command_queue_ == nullptr)
    this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  if ((this->event_queue_ == nullptr) || (this->command_queue_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

void AudioMixer::mix_audio_samples_without_clipping_(int16_t *media_buffer, int16_t *announcement_buffer,
                                                     int16_t *output_buffer, size_t samples_to_mix) {
  // We first test adding the two clips samples together and check for any clipping
  // We want the announcement volume to be consistent, regardless if media is playing or not
  // If there is clipping, we determine what factor we need to multiply that media sample by to avoid it
  // We take the smallest factor necessary for all the samples so the media volume is consistent on this batch
  // of samples
  // Note: This may not be the best approach. Adding 2 audio samples together makes both sound louder, even if
  // we are not clipping. As a result, the mixed announcement will sound louder (by around 3dB if the audio
  // streams are independent?) than if it were by itself.

  int16_t q15_scaling_factor = MAX_AUDIO_SAMPLE_VALUE;

  for (size_t i = 0; i < samples_to_mix; ++i) {
    int32_t added_sample = static_cast<int32_t>(media_buffer[i]) + static_cast<int32_t>(announcement_buffer[i]);

    if ((added_sample > MAX_AUDIO_SAMPLE_VALUE) || (added_sample < MIN_AUDIO_SAMPLE_VALUE)) {
      // The largest magnitude the media sample can be to avoid clipping (converted to Q30 fixed point)
      int32_t q30_media_sample_safe_max =
          static_cast<int32_t>(std::abs(MIN_AUDIO_SAMPLE_VALUE) - std::abs(announcement_buffer[i])) << 15;

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
    this->scale_audio_samples_(media_buffer, media_buffer, q15_scaling_factor, samples_to_mix);

    // Mix both stream by adding them together with no bitshift
    // The dsps_add functions have the following inputs:
    // (buffer 1, buffer 2, output buffer, number of samples, buffer 1 step, buffer 2 step, output, buffer step,
    // bitshift)
    dsps_add_s16(media_buffer, announcement_buffer, output_buffer, samples_to_mix, 1, 1, 1, 0);
  }
}

void AudioMixer::scale_audio_samples_(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                                      size_t samples_to_scale) {
  // Note the assembly dsps_mulc function has glitches if the input and output buffers are the same.
  for (int i = 0; i < samples_to_scale; i++) {
    int32_t acc = (int32_t) audio_samples[i] * (int32_t) scale_factor;
    output_buffer[i] = (int16_t) (acc >> 15);
  }
}

}  // namespace speaker
}  // namespace esphome
