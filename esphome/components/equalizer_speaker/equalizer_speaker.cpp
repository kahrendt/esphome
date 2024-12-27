#ifdef USE_ESP32

#include "equalizer_speaker.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace equalizer_speaker {

static const UBaseType_t EQUALIZER_TASK_PRIORITY = 1;

static const uint32_t RING_BUFFER_DURATION_MS = 100;
static const uint32_t TRANSFER_BUFFER_DURATION_MS = 50;
static const size_t TASK_DELAY_MS = 25;

static const uint32_t TASK_STACK_SIZE = 3072;

static const char *const TAG = "equalizer_speaker";

enum EqualizerEventGroupBits : uint32_t {
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

void EqualizerSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
}

void EqualizerSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & EqualizerEventGroupBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting equalizer task");
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::STATE_STARTING);
  }

  if (event_group_bits & EqualizerEventGroupBits::ERR_ESP_NO_MEM) {
    this->status_set_error("Equalizer task failed to allocate the internal buffers");
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::ERR_ESP_NO_MEM);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (event_group_bits & EqualizerEventGroupBits::ERR_ESP_NOT_SUPPORTED) {
    this->status_set_error("Cannot resample due to an unsupported audio stream");
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::ERR_ESP_NOT_SUPPORTED);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (event_group_bits & EqualizerEventGroupBits::ERR_ESP_FAIL) {
    this->status_set_error("Equalizer task failed");
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::ERR_ESP_FAIL);
    this->state_ = speaker::STATE_STOPPING;
  }

  if (event_group_bits & EqualizerEventGroupBits::STATE_RUNNING) {
    ESP_LOGD(TAG, "Started Equalizer task");
    this->status_clear_error();
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::STATE_RUNNING);
  }
  if (event_group_bits & EqualizerEventGroupBits::STATE_STOPPING) {
    ESP_LOGD(TAG, "Stopping Equalizer task");
    xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::STATE_STOPPING);
  }
  if (event_group_bits & EqualizerEventGroupBits::STATE_STOPPED) {
    if (!this->task_created_) {
      ESP_LOGD(TAG, "Stopped Equalizer task");
      xEventGroupClearBits(this->event_group_, EqualizerEventGroupBits::ALL_BITS);
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
            this->status_set_error("Failed to start equalizer: equalizer task failed to start");
            break;
          default:
            this->status_set_error("Failed to start equalizer");
            break;
        }

        this->state_ = speaker::STATE_STOPPING;
      }
      break;
    }
    case speaker::STATE_RUNNING:
      if (this->output_speaker_->is_stopped()) {
        // this->state_ = speaker::STATE_STOPPING;
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

size_t EqualizerSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }

  size_t bytes_written = 0;

  if (this->ring_buffer_.use_count() == 1) {
    std::shared_ptr<RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    bytes_written = temp_ring_buffer->write_without_replacement(data, length, ticks_to_wait);
  }

  return bytes_written;
}

void EqualizerSpeaker::start() { this->state_ = speaker::STATE_STARTING; }

esp_err_t EqualizerSpeaker::start_() {
  this->output_speaker_->set_audio_stream_info(this->audio_stream_info_);

  if (this->task_handle_ == nullptr) {
    xTaskCreate(this->equalizer_task, "equalize", TASK_STACK_SIZE, (void *) this, EQUALIZER_TASK_PRIORITY,
                &this->task_handle_);

    if (this->task_handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
  }

  this->output_speaker_->start();

  return ESP_OK;
}

void EqualizerSpeaker::stop() { this->state_ = speaker::STATE_STOPPING; }

void EqualizerSpeaker::stop_() {
  if (this->task_handle_ != nullptr) {
    xEventGroupSetBits(this->event_group_, EqualizerEventGroupBits::COMMAND_STOP);
  }
  this->output_speaker_->stop();
}

void EqualizerSpeaker::finish() { this->output_speaker_->finish(); }

bool EqualizerSpeaker::has_buffered_data() const {
  bool has_ring_buffer_data = false;
  if (this->ring_buffer_.use_count() > 0) {
    has_ring_buffer_data = (this->ring_buffer_.lock()->available() > 0);
  }
  return (has_ring_buffer_data || this->output_speaker_->has_buffered_data());
}

void EqualizerSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->output_speaker_->set_mute_state(mute_state);
}

void EqualizerSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->output_speaker_->set_volume(volume);
}

void EqualizerSpeaker::equalizer_task(void *params) {
  EqualizerSpeaker *this_equalizer = (EqualizerSpeaker *) params;

  this_equalizer->task_created_ = true;
  xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::STATE_STARTING);

  std::unique_ptr<audio::AudioSourceTransferBuffer> input_transfer_buffer =
      audio::AudioSourceTransferBuffer::create(25 * this_equalizer->audio_stream_info_.get_bytes_per_ms());
  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(25 * this_equalizer->audio_stream_info_.get_bytes_per_ms());

  esp_err_t err = ESP_OK;
  // TODO: Verify it was created
  if (err == ESP_OK) {
    std::shared_ptr<RingBuffer> temp_ring_buffer =
        RingBuffer::create(RING_BUFFER_DURATION_MS * this_equalizer->audio_stream_info_.get_bytes_per_ms());

    if (temp_ring_buffer.use_count() == 0) {
      err = ESP_ERR_NO_MEM;
    } else {
      this_equalizer->ring_buffer_ = temp_ring_buffer;
      input_transfer_buffer->set_source(this_equalizer->ring_buffer_);
    }
  }

  output_transfer_buffer->set_sink(this_equalizer->output_speaker_);

  if (err == ESP_OK) {
    xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::STATE_RUNNING);
  } else if (err == ESP_ERR_NO_MEM) {
    xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::ERR_ESP_FAIL);
  } else if (err == ESP_ERR_NOT_SUPPORTED) {
    xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::ERR_ESP_NOT_SUPPORTED);
  }

  std::unique_ptr<equalizer::Equalizer> equalizer =
      std::make_unique<equalizer::Equalizer>(25 * this_equalizer->audio_stream_info_.get_frames_per_ms());
  equalizer->initialize(this_equalizer->audio_stream_info_.channels);

  const size_t bytes_per_frame = this_equalizer->audio_stream_info_.get_bytes_per_frame();

  while (err == ESP_OK) {
    uint32_t event_bits = xEventGroupGetBits(this_equalizer->event_group_);

    if (event_bits & EqualizerEventGroupBits::COMMAND_STOP) {
      break;
    }

    output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(10));
    input_transfer_buffer->transfer_data_from_source(pdMS_TO_TICKS(10));

    const size_t bytes_to_process = std::min(input_transfer_buffer->available(), output_transfer_buffer->free());
    const size_t frames_to_process = bytes_to_process / bytes_per_frame;

    // printf("available =%d; free=%d\n", input_transfer_buffer->available(), output_transfer_buffer->free());

    if (frames_to_process > 0) {
      uint32_t clipped_samples = 0;

      equalizer->equalize(reinterpret_cast<int16_t *>(input_transfer_buffer->get_buffer_start()),
                          reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), frames_to_process,
                          clipped_samples);
      input_transfer_buffer->decrease_buffer_length(frames_to_process * bytes_per_frame);
      output_transfer_buffer->increase_buffer_length(frames_to_process * bytes_per_frame);
      // printf("processed %d frames\n", frames_to_process);
    } else {
      delay(20);
    }
  }

  xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::STATE_STOPPING);
  // resampler.reset();
  xEventGroupSetBits(this_equalizer->event_group_, EqualizerEventGroupBits::STATE_STOPPED);
  this_equalizer->task_created_ = false;
  vTaskDelete(nullptr);
}

}  // namespace equalizer_speaker
}  // namespace esphome

#endif
