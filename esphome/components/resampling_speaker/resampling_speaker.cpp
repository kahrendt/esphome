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
  ERR_ESP_NOT_SUPPORTED = (1 << 20),
  ERR_ESP_FAIL = (1 << 21),
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
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & ResamplingEventGroupBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting resampler task");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_STARTING);
  }

  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_NO_MEM) {
    this->status_set_error("Resampler task failed to allocate the internal buffers");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_NO_MEM);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED) {
    this->status_set_error("Cannot resample due to an unsupported audio stream");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_FAIL) {
    this->status_set_error("Resampler task failed");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_FAIL);
    this->state_ = speaker::STATE_STOPPING;
  }

  if (event_group_bits & ResamplingEventGroupBits::STATE_RUNNING) {
    ESP_LOGD(TAG, "Started resampler task");
    this->status_clear_error();
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_RUNNING);
  }
  if (event_group_bits & ResamplingEventGroupBits::STATE_STOPPING) {
    ESP_LOGD(TAG, "Stopping resampler task");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_STOPPING);
  }
  if (event_group_bits & ResamplingEventGroupBits::STATE_STOPPED) {
    if (!this->task_created_) {
      ESP_LOGD(TAG, "Stopped resampler task");
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ALL_BITS);
      this->task_handle_ = nullptr;
    }
  }

  switch (this->state_) {
    case speaker::STATE_STARTING: {
      esp_err_t err = this->start_();
      if (err == ESP_OK) {
        this->status_clear_error();
        this->state_ = speaker::STATE_RUNNING;
      } else {
        switch (err) {
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
      if (this->output_speaker_->is_stopped()) {
        this->state_ = speaker::STATE_STOPPING;
      }

      break;
    case speaker::STATE_STOPPING:
      this->stop_();
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
  if ((this->output_speaker_->is_running()) && (!this->requires_resampling_())) {
    bytes_written = this->output_speaker_->play(data, length, ticks_to_wait);
  } else {
    if (this->ring_buffer_.use_count() == 1) {
      std::shared_ptr<RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
      bytes_written = temp_ring_buffer->write_without_replacement(data, length, ticks_to_wait);
    }
  }

  return bytes_written;
}

void ResamplingSpeaker::start() { this->state_ = speaker::STATE_STARTING; }

esp_err_t ResamplingSpeaker::start_() {
  audio::AudioStreamInfo resampled_stream_info = this->audio_stream_info_;
  resampled_stream_info.sample_rate = this->target_sample_rate_;

  this->output_speaker_->set_audio_stream_info(resampled_stream_info);
  this->output_speaker_->start();

  if (this->requires_resampling_()) {
    // Start the resampler task to handle converting sample rates

    if (this->task_handle_ == nullptr) {
      xTaskCreate(this->resample_task, "resample", TASK_STACK_SIZE, (void *) this, RESAMPLER_TASK_PRIORITY,
                  &this->task_handle_);

      if (this->task_handle_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
      }
    }
  }

  return ESP_OK;
}

void ResamplingSpeaker::stop() { this->state_ = speaker::STATE_STOPPING; }

void ResamplingSpeaker::stop_() {
  if (this->task_handle_ != nullptr) {
    xEventGroupSetBits(this->event_group_, ResamplingEventGroupBits::COMMAND_STOP);
  }
  this->output_speaker_->stop();
}

void ResamplingSpeaker::finish() { this->output_speaker_->finish(); }

bool ResamplingSpeaker::has_buffered_data() const {
  bool has_ring_buffer_data = false;
  if (this->requires_resampling_() && (this->ring_buffer_.use_count() > 0)) {
    has_ring_buffer_data = (this->ring_buffer_.lock()->available() > 0);
  }
  return (has_ring_buffer_data || this->output_speaker_->has_buffered_data());
}

void ResamplingSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->output_speaker_->set_mute_state(mute_state);
}

void ResamplingSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->output_speaker_->set_volume(volume);
}

void ResamplingSpeaker::resample_task(void *params) {
  ResamplingSpeaker *this_resampler = (ResamplingSpeaker *) params;

  this_resampler->task_created_ = true;
  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STARTING);

  audio::AudioStreamInfo resampled_stream_info = this_resampler->audio_stream_info_;
  resampled_stream_info.sample_rate = this_resampler->target_sample_rate_;

  std::unique_ptr<audio::AudioResampler> resampler = std::make_unique<audio::AudioResampler>(
      TRANSFER_BUFFER_DURATION_MS * this_resampler->audio_stream_info_.get_bytes_per_ms(),
      TRANSFER_BUFFER_DURATION_MS * resampled_stream_info.get_bytes_per_ms());

  esp_err_t err = resampler->start(this_resampler->audio_stream_info_, this_resampler->target_sample_rate_);

  if (err == ESP_OK) {
    std::shared_ptr<RingBuffer> temp_ring_buffer =
        RingBuffer::create(MIXER_INPUT_RING_BUFFER_DURATION_MS * this_resampler->audio_stream_info_.get_bytes_per_ms());

    if (temp_ring_buffer.use_count() == 0) {
      err = ESP_ERR_NO_MEM;
    } else {
      this_resampler->ring_buffer_ = temp_ring_buffer;
      resampler->add_source(this_resampler->ring_buffer_);

      this_resampler->output_speaker_->set_audio_stream_info(resampled_stream_info);
      resampler->add_sink(this_resampler->output_speaker_);
    }
  }

  if (err == ESP_OK) {
    xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_RUNNING);
  } else if (err == ESP_ERR_NO_MEM) {
    xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_FAIL);
  } else if (err == ESP_ERR_NOT_SUPPORTED) {
    xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED);
  }

  while (err == ESP_OK) {
    uint32_t event_bits = xEventGroupGetBits(this_resampler->event_group_);

    if (event_bits & ResamplingEventGroupBits::COMMAND_STOP) {
      break;
    }

    // Stop gracefully if the decoder is done
    audio::AudioResamplerState resampler_state = resampler->resample(false);

    if (resampler_state == audio::AudioResamplerState::FINISHED) {
      break;
    } else if (resampler_state == audio::AudioResamplerState::FAILED) {
      xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_FAIL);
      break;
    }
  }

  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STOPPING);
  resampler.reset();
  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STOPPED);
  this_resampler->task_created_ = false;
  vTaskDelete(nullptr);
}

}  // namespace resampling_speaker
}  // namespace esphome

#endif
