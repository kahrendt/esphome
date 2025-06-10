#include "i2s_audio_speaker.h"

#ifdef USE_ESP32

#ifdef USE_I2S_LEGACY
#include <driver/i2s.h>
#else
#include <driver/i2s_std.h>
#endif

#include "esphome/components/audio/audio.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "esp_timer.h"

namespace esphome {
namespace i2s_audio {

static const uint8_t DMA_BUFFER_DURATION_MS = 15;
static const size_t DMA_BUFFERS_COUNT = 4;

static const size_t TASK_DELAY_MS = DMA_BUFFER_DURATION_MS * DMA_BUFFERS_COUNT / 2;

static const size_t TASK_STACK_SIZE = 4096;
static const ssize_t TASK_PRIORITY = 18;

static const size_t I2S_EVENT_QUEUE_COUNT = DMA_BUFFERS_COUNT + 1;

static const char *const TAG = "i2s_audio.speaker";

struct i2s_dma_write_info {
  int64_t timestamp;
  uint32_t frames;
};

enum SpeakerEventGroupBits : uint32_t {
  COMMAND_START = (1 << 0),            // indicates loop should start speaker task
  COMMAND_STOP = (1 << 1),             // stops the speaker task
  COMMAND_STOP_GRACEFULLY = (1 << 2),  // Stops the speaker task once all data has been written

  TASK_STARTING = (1 << 10),
  TASK_RUNNING = (1 << 11),
  TASK_STOPPING = (1 << 12),
  TASK_STOPPED = (1 << 13),

  ERR_ESP_NO_MEM = (1 << 19),
  ERR_ESP_INVALID_SIZE = (1 << 20),

  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

/// @brief Multiplies the input array of Q15 numbers by a Q15 constant factor
///
/// Based on `dsps_mulc_s16_ansi` from the esp-dsp library:
/// https://github.com/espressif/esp-dsp/blob/master/modules/math/mulc/fixed/dsps_mulc_s16_ansi.c
/// (accessed on 2024-09-30).
/// @param input Array of Q15 numbers
/// @param output Array of Q15 numbers
/// @param len Length of array
/// @param c Q15 constant factor
static void q15_multiplication(const int16_t *input, int16_t *output, size_t len, int16_t c) {
  for (int i = 0; i < len; i++) {
    int32_t acc = (int32_t) input[i] * (int32_t) c;
    output[i] = (int16_t) (acc >> 15);
  }
}

// Lists the Q15 fixed point scaling factor for volume reduction.
// Has 100 values representing silence and a reduction [49, 48.5, ... 0.5, 0] dB.
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> Q15_VOLUME_SCALING_FACTORS = {
    0,     116,   122,   130,   137,   146,   154,   163,   173,   183,   194,   206,   218,   231,   244,
    259,   274,   291,   308,   326,   345,   366,   388,   411,   435,   461,   488,   517,   548,   580,
    615,   651,   690,   731,   774,   820,   868,   920,   974,   1032,  1094,  1158,  1227,  1300,  1377,
    1459,  1545,  1637,  1734,  1837,  1946,  2061,  2184,  2313,  2450,  2596,  2750,  2913,  3085,  3269,
    3462,  3668,  3885,  4116,  4360,  4619,  4893,  5183,  5490,  5816,  6161,  6527,  6914,  7324,  7758,
    8218,  8706,  9222,  9770,  10349, 10963, 11613, 12302, 13032, 13805, 14624, 15491, 16410, 17384, 18415,
    19508, 20665, 21891, 23189, 24565, 26022, 27566, 29201, 30933, 32767};

void I2SAudioSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Running setup");

  this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }
}

void I2SAudioSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Speaker:\n"
                "  Pin: %d\n"
                "  Buffer duration: %" PRIu32,
                static_cast<int8_t>(this->dout_pin_), this->buffer_duration_ms_);
  if (this->timeout_.has_value()) {
    ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 " ms", this->timeout_.value());
  }
#ifdef USE_I2S_LEGACY
#if SOC_I2S_SUPPORTS_DAC
  ESP_LOGCONFIG(TAG, "  Internal DAC mode: %d", static_cast<int8_t>(this->internal_dac_mode_));
#endif
  ESP_LOGCONFIG(TAG, "  Communication format: %d", static_cast<int8_t>(this->i2s_comm_fmt_));
#else
  ESP_LOGCONFIG(TAG, "  Communication format: %s", this->i2s_comm_fmt_.c_str());
#endif
}

void I2SAudioSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if ((event_group_bits & SpeakerEventGroupBits::COMMAND_START) && (this->state_ == speaker::STATE_STOPPED)) {
    this->state_ = speaker::STATE_STARTING;
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::COMMAND_START);
  }

  // Handle the task's state
  if (event_group_bits & SpeakerEventGroupBits::TASK_STARTING) {
    ESP_LOGD(TAG, "Task started, attempting to allocate buffer");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::TASK_STARTING);
  }
  if (event_group_bits & SpeakerEventGroupBits::TASK_RUNNING) {
    ESP_LOGD(TAG, "Task is running and writing data");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::TASK_RUNNING);
    this->state_ = speaker::STATE_RUNNING;
  }
  if (event_group_bits & SpeakerEventGroupBits::TASK_STOPPING) {
    ESP_LOGD(TAG, "Task is stopping, deallocating buffer");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::TASK_STOPPING);
    this->state_ = speaker::STATE_STOPPING;
  }
  if (event_group_bits & SpeakerEventGroupBits::TASK_STOPPED) {
    ESP_LOGD(TAG, "Task finished, freeing resources and uninstalling I2S driver");

    vTaskDelete(this->speaker_task_handle_);
    this->speaker_task_handle_ = nullptr;

    this->stop_i2s_driver_();
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::ALL_BITS);
    this->status_clear_error();

    this->state_ = speaker::STATE_STOPPED;
  }

  // Log any errors encounted by the task
  if (event_group_bits & SpeakerEventGroupBits::ERR_ESP_NO_MEM) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    xEventGroupClearBits(this->event_group_, SpeakerEventGroupBits::ERR_ESP_NO_MEM);
  }
  if (event_group_bits & SpeakerEventGroupBits::ERR_ESP_INVALID_SIZE) {
    ESP_LOGE(TAG, "Unable write all data to the I2S bus");
    // The error bit is cleared by the task on the next successful write
  }

  // Handle the speaker's state
  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (this->status_has_error()) {
        break;
      }

      if (this->start_i2s_driver_(this->audio_stream_info_) != ESP_OK) {
        this->status_momentary_error("I2S driver failed to start, attempting again in 1 second", 1000);
        break;
      }

      if (this->speaker_task_handle_ == nullptr) {
        xTaskCreate(I2SAudioSpeaker::speaker_task, "speaker_task", TASK_STACK_SIZE, (void *) this, TASK_PRIORITY,
                    &this->speaker_task_handle_);

        if (this->speaker_task_handle_ == nullptr) {
          this->status_momentary_error("Task failed to start, attempting again in 1 second", 1000);
          this->stop_i2s_driver_();  // Stops the driver to return the lock; will be reloaded in next attempt
        }
      }
      break;
    case speaker::STATE_RUNNING:
      break;
    case speaker::STATE_STOPPING:
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

void I2SAudioSpeaker::set_volume(float volume) {
  this->volume_ = volume;
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_ != nullptr) {
    if (volume > 0.0) {
      this->audio_dac_->set_mute_off();
    }
    this->audio_dac_->set_volume(volume);
  } else
#endif
  {
    // Fallback to software volume control by using a Q15 fixed point scaling factor
    ssize_t decibel_index = remap<ssize_t, float>(volume, 0.0f, 1.0f, 0, Q15_VOLUME_SCALING_FACTORS.size() - 1);
    this->q15_volume_factor_ = Q15_VOLUME_SCALING_FACTORS[decibel_index];
  }
}

void I2SAudioSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_) {
    if (mute_state) {
      this->audio_dac_->set_mute_on();
    } else {
      this->audio_dac_->set_mute_off();
    }
  } else
#endif
  {
    if (mute_state) {
      // Fallback to software volume control and scale by 0
      this->q15_volume_factor_ = 0;
    } else {
      // Revert to previous volume when unmuting
      this->set_volume(this->volume_);
    }
  }
}

size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Setup failed; cannot play audio");
    return 0;
  }
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }

  if ((this->state_ != speaker::STATE_RUNNING) || (this->audio_ring_buffer_.use_count() != 1)) {
    // Unable to write data to a running speaker, so delay the max amount of time so it can get ready
    vTaskDelay(ticks_to_wait);
    ticks_to_wait = 0;
  }

  size_t bytes_written = 0;
  if ((this->state_ == speaker::STATE_RUNNING) && (this->audio_ring_buffer_.use_count() == 1)) {
    // Only one owner of the ring buffer (the speaker task), so the ring buffer is allocated and no other components are
    // attempting to write to it.

    // Temporarily share ownership of the ring buffer so it won't be deallocated while writing
    std::shared_ptr<RingBuffer> temp_ring_buffer = this->audio_ring_buffer_;
    bytes_written = temp_ring_buffer->write_without_replacement((void *) data, length, ticks_to_wait);
  }

  return bytes_written;
}

bool I2SAudioSpeaker::has_buffered_data() const {
  if (this->audio_ring_buffer_ != nullptr) {
    return this->audio_ring_buffer_->available() > 0;
  }
  return false;
}

void I2SAudioSpeaker::speaker_task(void *params) {
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;

  const uint32_t dma_buffers_duration_ms = DMA_BUFFER_DURATION_MS * DMA_BUFFERS_COUNT;
  // Ensure ring buffer duration is at least the duration of all DMA buffers
  const uint32_t ring_buffer_duration = std::max(dma_buffers_duration_ms, this_speaker->buffer_duration_ms_);

  // The DMA buffers may have more bits per sample, so calculate buffer sizes based in the input audio stream info
  const size_t data_buffer_size = this_speaker->current_stream_info_.ms_to_bytes(dma_buffers_duration_ms);
  const size_t ring_buffer_size = this_speaker->current_stream_info_.ms_to_bytes(ring_buffer_duration);

  const size_t single_dma_buffer_input_size = data_buffer_size / DMA_BUFFERS_COUNT;

  xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_STARTING);

  if (this_speaker->allocate_buffers_(data_buffer_size, ring_buffer_size) != ESP_OK) {
    xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::ERR_ESP_NO_MEM);
  } else {
    xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_RUNNING);

    bool stop_gracefully = false;
    uint32_t last_data_received_time = millis();
    bool tx_dma_underflow = false;

    this_speaker->frames_written_ = 0;

    while (this_speaker->pause_state_ || !this_speaker->timeout_.has_value() ||
           (millis() - last_data_received_time) <= this_speaker->timeout_.value()) {
      uint32_t event_group_bits = xEventGroupGetBits(this_speaker->event_group_);

      if (event_group_bits & SpeakerEventGroupBits::COMMAND_STOP) {
        xEventGroupClearBits(this_speaker->event_group_, SpeakerEventGroupBits::COMMAND_STOP);
        break;
      }
      if (event_group_bits & SpeakerEventGroupBits::COMMAND_STOP_GRACEFULLY) {
        xEventGroupClearBits(this_speaker->event_group_, SpeakerEventGroupBits::COMMAND_STOP_GRACEFULLY);
        stop_gracefully = true;
      }

      if (this_speaker->audio_stream_info_ != this_speaker->current_stream_info_) {
        // Audio stream info changed, stop the speaker task so it will restart with the proper settings.
        break;
      }
#ifdef USE_I2S_LEGACY
      i2s_event_t i2s_event;
      while (xQueueReceive(this_speaker->i2s_event_queue_, &i2s_event, 0)) {
        if (i2s_event.type == I2S_EVENT_TX_Q_OVF) {
          tx_dma_underflow = true;
        }
      }
#else
      i2s_dma_write_info write_info;
      while (xQueueReceive(this_speaker->i2s_event_queue_, &write_info, 0)) {
        this_speaker->audio_output_callback_(write_info.frames, write_info.timestamp);
        if (this_speaker->frames_written_ == 0) {
          tx_dma_underflow = true;

          // TODO: Will this cause audible issues with certain DACs? It stops sending WCLK and BLCK signals...
          i2s_channel_disable(this_speaker->tx_handle_);
        }
      }
#endif

      if (this_speaker->pause_state_) {
        // Pause state is accessed atomically, so thread safe
        // Delay so the task yields, then skip transferring audio data
        delay(TASK_DELAY_MS);
        continue;
      }

      size_t bytes_read = this_speaker->audio_ring_buffer_->read((void *) this_speaker->data_buffer_, data_buffer_size,
                                                                 pdMS_TO_TICKS(TASK_DELAY_MS));

      if (bytes_read > 0) {
        if ((this_speaker->current_stream_info_.get_bits_per_sample() == 16) &&
            (this_speaker->q15_volume_factor_ < INT16_MAX)) {
          // Scale samples by the volume factor in place
          q15_multiplication((int16_t *) this_speaker->data_buffer_, (int16_t *) this_speaker->data_buffer_,
                             bytes_read / sizeof(int16_t), this_speaker->q15_volume_factor_);
        }

#ifdef USE_ESP32_VARIANT_ESP32
        // For ESP32 8/16 bit mono mode samples need to be switched.
        if (this_speaker->current_stream_info_.get_channels() == 1 &&
            this_speaker->current_stream_info_.get_bits_per_sample() <= 16) {
          size_t len = bytes_read / sizeof(int16_t);
          int16_t *tmp_buf = (int16_t *) this_speaker->data_buffer_;
          for (int i = 0; i < len; i += 2) {
            int16_t tmp = tmp_buf[i];
            tmp_buf[i] = tmp_buf[i + 1];
            tmp_buf[i + 1] = tmp;
          }
        }
#endif
        // Write the audio data to a single DMA buffer at a time to reduce latency for the audio duration played
        // callback.
        const uint32_t batches = (bytes_read + single_dma_buffer_input_size - 1) / single_dma_buffer_input_size;

        for (uint32_t i = 0; i < batches; ++i) {
          size_t bytes_written = 0;
          size_t bytes_to_write = std::min(single_dma_buffer_input_size, bytes_read);

#ifdef USE_I2S_LEGACY
          if (this_speaker->current_stream_info_.get_bits_per_sample() == (uint8_t) this_speaker->bits_per_sample_) {
            i2s_write(this_speaker->parent_->get_port(), this_speaker->data_buffer_ + i * single_dma_buffer_input_size,
                      bytes_to_write, &bytes_written, pdMS_TO_TICKS(DMA_BUFFER_DURATION_MS * 5));
          } else if (audio_stream_info.get_bits_per_sample() < (uint8_t) this_speaker->bits_per_sample_) {
            i2s_write_expand(this_speaker->parent_->get_port(),
                             this_speaker->data_buffer_ + i * single_dma_buffer_input_size, bytes_to_write,
                             audio_stream_info.get_bits_per_sample(), this_speaker->bits_per_sample_, &bytes_written,
                             pdMS_TO_TICKS(DMA_BUFFER_DURATION_MS * 5));
          }
#else
          while (xQueueReceive(this_speaker->i2s_event_queue_, &write_info, 0)) {
            this_speaker->audio_output_callback_(write_info.frames, write_info.timestamp);
            if (this_speaker->frames_written_ == 0) {
              tx_dma_underflow = true;

              // TODO: Will this cause audible issues with certain DACs? It stops sending WCLK and BLCK signals...
              i2s_channel_disable(this_speaker->tx_handle_);
            }
          }

          if (this_speaker->frames_written_ == 0) {
            i2s_channel_preload_data(this_speaker->tx_handle_,
                                     this_speaker->data_buffer_ + i * single_dma_buffer_input_size, bytes_to_write,
                                     &bytes_written);
            if (bytes_written > 0) {
              i2s_channel_enable(this_speaker->tx_handle_);
            }

          } else {
            i2s_channel_write(this_speaker->tx_handle_, this_speaker->data_buffer_ + i * single_dma_buffer_input_size,
                              bytes_to_write, &bytes_written, pdMS_TO_TICKS(DMA_BUFFER_DURATION_MS * 5));
          }

          this_speaker->frames_written_ += this_speaker->current_stream_info_.bytes_to_frames(bytes_written);

#endif

          // int64_t now = esp_timer_get_time();

          if (bytes_written != bytes_to_write) {
            xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::ERR_ESP_INVALID_SIZE);
          } else {
            xEventGroupClearBits(this_speaker->event_group_, SpeakerEventGroupBits::ERR_ESP_INVALID_SIZE);
          }
          bytes_read -= bytes_written;

          // this_speaker->audio_output_callback_(this_speaker->current_stream_info_.bytes_to_frames(bytes_written),
          //                                      now + dma_buffers_duration_ms * 1000);

          tx_dma_underflow = false;
          last_data_received_time = millis();
        }
      } else {
        // No data received
        if (stop_gracefully && tx_dma_underflow) {
          break;
        }
      }
    }
  }

  xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_STOPPING);

  this_speaker->deallocate_buffers_(data_buffer_size);

  xEventGroupSetBits(this_speaker->event_group_, SpeakerEventGroupBits::TASK_STOPPED);

  while (true) {
    // Continuously delay until the loop method deletes the task
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void I2SAudioSpeaker::start() {
  if (!this->is_ready() || this->is_failed() || this->status_has_error())
    return;
  if ((this->state_ == speaker::STATE_STARTING) || (this->state_ == speaker::STATE_RUNNING))
    return;

  xEventGroupSetBits(this->event_group_, SpeakerEventGroupBits::COMMAND_START);
}

void I2SAudioSpeaker::stop() { this->stop_(false); }

void I2SAudioSpeaker::finish() { this->stop_(true); }

void I2SAudioSpeaker::stop_(bool wait_on_empty) {
  if (this->is_failed())
    return;
  if (this->state_ == speaker::STATE_STOPPED)
    return;

  if (wait_on_empty) {
    xEventGroupSetBits(this->event_group_, SpeakerEventGroupBits::COMMAND_STOP_GRACEFULLY);
  } else {
    xEventGroupSetBits(this->event_group_, SpeakerEventGroupBits::COMMAND_STOP);
  }
}

esp_err_t I2SAudioSpeaker::allocate_buffers_(size_t data_buffer_size, size_t ring_buffer_size) {
  if (this->data_buffer_ == nullptr) {
    // Allocate data buffer for temporarily storing audio from the ring buffer before writing to the I2S bus
    RAMAllocator<uint8_t> allocator;
    this->data_buffer_ = allocator.allocate(data_buffer_size);
  }

  if (this->data_buffer_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (this->audio_ring_buffer_.use_count() == 0) {
    // Allocate ring buffer. Uses a shared_ptr to ensure it isn't improperly deallocated.
    this->audio_ring_buffer_ = RingBuffer::create(ring_buffer_size);
  }

  if (this->audio_ring_buffer_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

void I2SAudioSpeaker::deallocate_buffers_(size_t data_buffer_size) {
  this->audio_ring_buffer_.reset();  // Releases ownership of the shared_ptr

  if (this->data_buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->data_buffer_, data_buffer_size);
    this->data_buffer_ = nullptr;
  }
}

esp_err_t I2SAudioSpeaker::start_i2s_driver_(audio::AudioStreamInfo &audio_stream_info) {
  this->current_stream_info_ = audio_stream_info;  // store the stream info settings the driver will use

#ifdef USE_I2S_LEGACY
  if ((this->i2s_mode_ & I2S_MODE_SLAVE) && (this->sample_rate_ != audio_stream_info.get_sample_rate())) {  // NOLINT
#else
  if ((this->i2s_role_ & I2S_ROLE_SLAVE) && (this->sample_rate_ != audio_stream_info.get_sample_rate())) {  // NOLINT
#endif
    // Can't reconfigure I2S bus, so the sample rate must match the configured value
    ESP_LOGE(TAG, "Audio stream settings are not compatible with this I2S configuration");
    return ESP_ERR_NOT_SUPPORTED;
  }

#ifdef USE_I2S_LEGACY
  if ((i2s_bits_per_sample_t) audio_stream_info.get_bits_per_sample() > this->bits_per_sample_) {
#else
  if (this->slot_bit_width_ != I2S_SLOT_BIT_WIDTH_AUTO &&
      (i2s_slot_bit_width_t) audio_stream_info.get_bits_per_sample() > this->slot_bit_width_) {
#endif
    // Currently can't handle the case when the incoming audio has more bits per sample than the configured value
    ESP_LOGE(TAG, "Audio streams with more bits per sample than the I2S speaker's configuration is not supported");
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (!this->parent_->try_lock()) {
    ESP_LOGE(TAG, "Parent I2S bus not free");
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t dma_buffer_length = audio_stream_info.ms_to_frames(DMA_BUFFER_DURATION_MS);

#ifdef USE_I2S_LEGACY
  i2s_channel_fmt_t channel = this->channel_;

  if (audio_stream_info.get_channels() == 1) {
    if (this->channel_ == I2S_CHANNEL_FMT_ONLY_LEFT) {
      channel = I2S_CHANNEL_FMT_ONLY_LEFT;
    } else {
      channel = I2S_CHANNEL_FMT_ONLY_RIGHT;
    }
  } else if (audio_stream_info.get_channels() == 2) {
    channel = I2S_CHANNEL_FMT_RIGHT_LEFT;
  }

  i2s_driver_config_t config = {
    .mode = (i2s_mode_t) (this->i2s_mode_ | I2S_MODE_TX),
    .sample_rate = audio_stream_info.get_sample_rate(),
    .bits_per_sample = this->bits_per_sample_,
    .channel_format = channel,
    .communication_format = this->i2s_comm_fmt_,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUFFERS_COUNT,
    .dma_buf_len = (int) dma_buffer_length,
    .use_apll = this->use_apll_,
    .tx_desc_auto_clear = true,
    .fixed_mclk = I2S_PIN_NO_CHANGE,
    .mclk_multiple = this->mclk_multiple_,
    .bits_per_chan = this->bits_per_channel_,
#if SOC_I2S_SUPPORTS_TDM
    .chan_mask = (i2s_channel_t) (I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
    .total_chan = 2,
    .left_align = false,
    .big_edin = false,
    .bit_order_msb = false,
    .skip_msk = false,
#endif
  };
#if SOC_I2S_SUPPORTS_DAC
  if (this->internal_dac_mode_ != I2S_DAC_CHANNEL_DISABLE) {
    config.mode = (i2s_mode_t) (config.mode | I2S_MODE_DAC_BUILT_IN);
  }
#endif

  esp_err_t err =
      i2s_driver_install(this->parent_->get_port(), &config, I2S_EVENT_QUEUE_COUNT, &this->i2s_event_queue_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install I2S legacy driver");
    // Failed to install the driver, so unlock the I2S port
    this->parent_->unlock();
    return err;
  }

#if SOC_I2S_SUPPORTS_DAC
  if (this->internal_dac_mode_ == I2S_DAC_CHANNEL_DISABLE) {
#endif
    i2s_pin_config_t pin_config = this->parent_->get_pin_config();
    pin_config.data_out_num = this->dout_pin_;

    err = i2s_set_pin(this->parent_->get_port(), &pin_config);
#if SOC_I2S_SUPPORTS_DAC
  } else {
    i2s_set_dac_mode(this->internal_dac_mode_);
  }
#endif

  if (err != ESP_OK) {
    // Failed to set the data out pin, so uninstall the driver and unlock the I2S port
    ESP_LOGE(TAG, "Failed to set the data out pin");
    i2s_driver_uninstall(this->parent_->get_port());
    this->parent_->unlock();
  }
#else
  i2s_chan_config_t chan_cfg = {
      .id = this->parent_->get_port(),
      .role = this->i2s_role_,
      .dma_desc_num = DMA_BUFFERS_COUNT,
      .dma_frame_num = dma_buffer_length,
      .auto_clear = true,
  };
  /* Allocate a new TX channel and get the handle of this channel */
  esp_err_t err = i2s_new_channel(&chan_cfg, &this->tx_handle_, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to allocate new I2S channel");
    this->parent_->unlock();
    return err;
  }

  i2s_clock_src_t clk_src = I2S_CLK_SRC_DEFAULT;
#ifdef I2S_CLK_SRC_APLL
  if (this->use_apll_) {
    clk_src = I2S_CLK_SRC_APLL;
  }
#endif
  i2s_std_gpio_config_t pin_config = this->parent_->get_pin_config();

  i2s_std_clk_config_t clk_cfg = {
      .sample_rate_hz = audio_stream_info.get_sample_rate(),
      .clk_src = clk_src,
      .mclk_multiple = this->mclk_multiple_,
  };

  i2s_slot_mode_t slot_mode = this->slot_mode_;
  i2s_std_slot_mask_t slot_mask = this->std_slot_mask_;
  if (audio_stream_info.get_channels() == 1) {
    slot_mode = I2S_SLOT_MODE_MONO;
  } else if (audio_stream_info.get_channels() == 2) {
    slot_mode = I2S_SLOT_MODE_STEREO;
    slot_mask = I2S_STD_SLOT_BOTH;
  }

  i2s_std_slot_config_t std_slot_cfg;
  if (this->i2s_comm_fmt_ == "std") {
    std_slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t) audio_stream_info.get_bits_per_sample(), slot_mode);
  } else if (this->i2s_comm_fmt_ == "pcm") {
    std_slot_cfg =
        I2S_STD_PCM_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t) audio_stream_info.get_bits_per_sample(), slot_mode);
  } else {
    std_slot_cfg =
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t) audio_stream_info.get_bits_per_sample(), slot_mode);
  }
#ifdef USE_ESP32_VARIANT_ESP32
  // There seems to be a bug on the ESP32 (non-variant) platform where setting the slot bit width higher then the bits
  // per sample causes the audio to play too fast. Setting the ws_width to the configured slot bit width seems to
  // make it play at the correct speed while sending more bits per slot.
  if (this->slot_bit_width_ != I2S_SLOT_BIT_WIDTH_AUTO) {
    uint32_t configured_bit_width = static_cast<uint32_t>(this->slot_bit_width_);
    std_slot_cfg.ws_width = configured_bit_width;
    if (configured_bit_width > 16) {
      std_slot_cfg.msb_right = false;
    }
  }
#else
  std_slot_cfg.slot_bit_width = this->slot_bit_width_;
#endif
  std_slot_cfg.slot_mask = slot_mask;

  pin_config.dout = this->dout_pin_;

  i2s_std_config_t std_cfg = {
      .clk_cfg = clk_cfg,
      .slot_cfg = std_slot_cfg,
      .gpio_cfg = pin_config,
  };
  /* Initialize the channel */
  err = i2s_channel_init_std_mode(this->tx_handle_, &std_cfg);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize channel");
    i2s_del_channel(this->tx_handle_);
    this->parent_->unlock();
    return err;
  }
  if (this->i2s_event_queue_ == nullptr) {
    this->i2s_event_queue_ = xQueueCreate(10, sizeof(i2s_dma_write_info));
  }
  const i2s_event_callbacks_t callbacks = {
      .on_sent = i2s_on_sent_cb,
      // .on_send_q_ovf = i2s_overflow_cb,
  };

  i2s_channel_register_event_callback(this->tx_handle_, &callbacks, this);

  /* Before reading data, start the TX channel first */
  // i2s_channel_enable(this->tx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable I2S channel");
    i2s_del_channel(this->tx_handle_);
    this->parent_->unlock();
  }
#endif

  return err;
}

#ifndef USE_I2S_LEGACY
bool IRAM_ATTR I2SAudioSpeaker::i2s_overflow_cb(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) user_ctx;
  bool overflow = true;
  // xQueueOverwrite(this_speaker->i2s_event_queue_, &overflow);
  return false;
}

bool IRAM_ATTR I2SAudioSpeaker::i2s_on_sent_cb(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
  int64_t now = esp_timer_get_time();
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) user_ctx;

  // TODO: This may not account for expanding to higher bits per sample...
  uint32_t frames_sent =
      std::min(this_speaker->current_stream_info_.bytes_to_frames(event->size), this_speaker->frames_written_);
  this_speaker->frames_written_ -= frames_sent;

  i2s_dma_write_info write_info = {.timestamp = now, .frames = frames_sent};

  xQueueSend(this_speaker->i2s_event_queue_, &write_info, 0);

  return false;
}
#endif

void I2SAudioSpeaker::stop_i2s_driver_() {
#ifdef USE_I2S_LEGACY
  i2s_driver_uninstall(this->parent_->get_port());
#else
  i2s_channel_disable(this->tx_handle_);
  i2s_del_channel(this->tx_handle_);
#endif

  this->parent_->unlock();
}

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32
