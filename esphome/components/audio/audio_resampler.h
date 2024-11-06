#pragma once

#include "biquad.h"
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
  ~AudioResampler();

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
  /// @return ESP_OK if it is able to convert the incoming stream or an error otherwise
  esp_err_t start(AudioStreamInfo &stream_info, uint32_t target_sample_rate, ResampleInfo &resample_info);

  AudioResamplerState resample(bool stop_gracefully);

 protected:
  esp_err_t allocate_buffers_(bool allocate_float_buffers);

  std::unique_ptr<AudioSourceTransferBuffer> input_transfer_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;

  uint8_t *output_buffer_current_{nullptr};

  float *float_input_buffer_{nullptr};
  float *float_input_buffer_current_{nullptr};
  size_t float_input_buffer_length_;
  size_t input_buffer_size_;

  float *float_output_buffer_{nullptr};
  float *float_output_buffer_current_{nullptr};
  size_t float_output_buffer_length_;
  size_t output_buffer_size_;

  AudioStreamInfo stream_info_;
  ResampleInfo resample_info_;

  Resample *resampler_{nullptr};

  Biquad lowpass_[2][2];
  BiquadCoefficients lowpass_coeff_;

  float sample_ratio_{1.0};
  float lowpass_ratio_{1.0};
  uint8_t channel_factor_{1};

  bool pre_filter_{false};
  bool post_filter_{false};
};

}  // namespace audio
}  // namespace esphome
