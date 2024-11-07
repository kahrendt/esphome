#pragma once

// #include "biquad.h"
#include "resampler.h"

#include "audio.h"
#include "audio_transfer_buffer.h"

#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace audio {

enum class AudioResamplerState : uint8_t {
  INITIALIZED = 0,
  RESAMPLING,
  FINISHED,
  FAILED,
};

struct ResampleInfo {
  bool resample;
  bool mono_to_stereo;
};

class AudioResampler {
 public:
  AudioResampler(size_t input_buffer_size, size_t output_buffer_size)
      : input_buffer_size_(input_buffer_size), output_buffer_size_(output_buffer_size) {
    this->input_transfer_buffer_ = AudioSourceTransferBuffer::create(input_buffer_size);
    this->output_transfer_buffer_ = AudioSinkTransferBuffer::create(output_buffer_size);
  }

  esp_err_t add_input_ring_buffer(std::weak_ptr<esphome::RingBuffer> input_ring_buffer) {
    if (this->input_transfer_buffer_ != nullptr) {
      this->input_transfer_buffer_->set_source(input_ring_buffer);
      return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
  }

  esp_err_t add_output_ring_buffer(std::weak_ptr<esphome::RingBuffer> output_ring_buffer) {
    if (this->output_transfer_buffer_ != nullptr) {
      this->output_transfer_buffer_->set_sink(output_ring_buffer);
      return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
  }

  /// @brief Sets up the various bits necessary to resample
  /// @param stream_info the incoming sample rate, bits per sample, and number of channels
  /// @param target_sample_rate the necessary sample rate to convert to
  /// @param resample_info ResampleInfo object passed by reference that indicates which resampling processes are applied
  /// @return ESP_OK if it is able to convert the incoming stream or an error otherwise
  esp_err_t start(AudioStreamInfo &stream_info, uint32_t target_sample_rate, ResampleInfo &resample_info);

  AudioResamplerState resample(bool stop_gracefully);

 protected:
  std::unique_ptr<AudioSourceTransferBuffer> input_transfer_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;

  size_t input_buffer_size_;
  size_t output_buffer_size_;

  AudioStreamInfo stream_info_;
  ResampleInfo resample_info_;

  std::unique_ptr<resampler::Resampler> resampler_;
};

}  // namespace audio
}  // namespace esphome
