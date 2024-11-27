#ifdef USE_ESP32

#include "audio_resampler.h"

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace audio {

static const size_t NUM_TAPS = 16;
static const size_t NUM_FILTERS = 16;

// The resampling library's implementation hardcodes the bits per sample
static const uint8_t OUTPUT_BYTES_PER_SAMPLE = sizeof(int16_t);
static const uint8_t OUTPUT_BITS_PER_SAMPLE = 8 * OUTPUT_BYTES_PER_SAMPLE;

static const size_t READ_WRITE_TIMEOUT_MS = 20;

AudioResampler::AudioResampler(size_t input_buffer_size, size_t output_buffer_size)
    : input_buffer_size_(input_buffer_size), output_buffer_size_(output_buffer_size) {
  this->input_transfer_buffer_ = AudioSourceTransferBuffer::create(input_buffer_size);
  this->output_transfer_buffer_ = AudioSinkTransferBuffer::create(output_buffer_size);
}

esp_err_t AudioResampler::add_source(std::weak_ptr<RingBuffer> input_ring_buffer) {
  if (this->input_transfer_buffer_ != nullptr) {
    this->input_transfer_buffer_->set_source(input_ring_buffer);
    return ESP_OK;
  }
  return ESP_ERR_NO_MEM;
}

esp_err_t AudioResampler::add_sink(std::weak_ptr<RingBuffer> output_ring_buffer) {
  if (this->output_transfer_buffer_ != nullptr) {
    this->output_transfer_buffer_->set_sink(output_ring_buffer);
    return ESP_OK;
  }
  return ESP_ERR_NO_MEM;
}

#ifdef USE_SPEAKER
esp_err_t AudioResampler::add_sink(speaker::Speaker *speaker) {
  if (this->output_transfer_buffer_ != nullptr) {
    this->output_transfer_buffer_->set_sink(speaker);
    return ESP_OK;
  }
  return ESP_ERR_NO_MEM;
}
#endif

esp_err_t AudioResampler::start(AudioStreamInfo &input_stream_info, uint32_t target_sample_rate) {
  this->input_stream_info_ = input_stream_info;

  if ((this->input_transfer_buffer_ == nullptr) || (this->output_transfer_buffer_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  if (input_stream_info_.bits_per_sample != OUTPUT_BITS_PER_SAMPLE) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (input_stream_info.sample_rate != target_sample_rate) {
    this->resampler_ =
        make_unique<resampler::Resampler>(this->input_buffer_size_ / input_stream_info.get_bytes_per_sample(),
                                          this->output_buffer_size_ / input_stream_info.get_bytes_per_sample());

    uint16_t number_of_filters = NUM_FILTERS;
    bool subsample_interpolate = true;
    if (target_sample_rate % input_stream_info.sample_rate == 0) {
      // Upsampling by an integer factor
      number_of_filters = target_sample_rate / input_stream_info.sample_rate;
      subsample_interpolate = false;
    }

    if (input_stream_info.sample_rate % target_sample_rate == 0) {
      // Downsampling by an integer factor
      subsample_interpolate = false;
    }

    if (!this->resampler_->initialize(static_cast<float>(target_sample_rate),
                                      static_cast<float>(input_stream_info.sample_rate), input_stream_info.channels,
                                      (uint16_t) NUM_TAPS, number_of_filters, false, subsample_interpolate)) {
      // Failed to allocate the resampler's internal buffers
      return ESP_ERR_NO_MEM;
    }

    this->output_stream_info_ = this->input_stream_info_;
    this->output_stream_info_.sample_rate = target_sample_rate;
  } else {
    this->output_stream_info_ = this->input_stream_info_;
  }

  return ESP_OK;
}

AudioResamplerState AudioResampler::resample(bool stop_gracefully) {
  if (stop_gracefully) {
    if (!this->input_transfer_buffer_->has_buffered_data() && (this->output_transfer_buffer_->available() == 0)) {
      return AudioResamplerState::FINISHED;
    }
  }

  if (!this->pause_output_) {
    // Move audio data to the sink/from the source
    this->output_transfer_buffer_->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
  } else {
    // If paused, block to avoid wasting CPU resources
    delay(READ_WRITE_TIMEOUT_MS);
  }
  this->input_transfer_buffer_->transfer_data_from_source(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

  if (this->input_transfer_buffer_->available() == 0) {
    // No samples available to process
    return AudioResamplerState::RESAMPLING;
  }

  int16_t *output_buffer = reinterpret_cast<int16_t *>(this->output_transfer_buffer_->get_buffer_end());

  // Samples are indiviudal int16 values with a size of 2 bytes. Frames include a sample for each channel.
  const size_t bytes_free = this->output_transfer_buffer_->free();
  const size_t frames_free = bytes_free / this->output_stream_info_.get_bytes_per_frame();

  const size_t bytes_available = this->input_transfer_buffer_->available();
  const size_t frames_available = bytes_available / this->input_stream_info_.get_bytes_per_frame();

  if (this->input_stream_info_.sample_rate != this->output_stream_info_.sample_rate) {
    size_t frames_used = 0;
    size_t frames_generated = 0;

    this->resampler_->resample(reinterpret_cast<int16_t *>(this->input_transfer_buffer_->get_buffer_start()),
                               output_buffer, frames_available, frames_free, frames_used, frames_generated);

    this->input_transfer_buffer_->decrease_buffer_length(frames_used * this->input_stream_info_.get_bytes_per_frame());
    this->output_transfer_buffer_->increase_buffer_length(frames_generated *
                                                          this->output_stream_info_.get_bytes_per_frame());
  } else {
    // No resampling required, copy samples directly to the output transfer buffer

    const size_t bytes_to_transfer = std::min(frames_free * this->output_stream_info_.get_bytes_per_frame(),
                                              frames_available * this->input_stream_info_.get_bytes_per_frame());

    std::memcpy((void *) output_buffer, (void *) this->input_transfer_buffer_->get_buffer_start(), bytes_to_transfer);

    this->input_transfer_buffer_->decrease_buffer_length(bytes_to_transfer);
    this->output_transfer_buffer_->increase_buffer_length(bytes_to_transfer);
  }

  return AudioResamplerState::RESAMPLING;
}

}  // namespace audio
}  // namespace esphome

#endif
