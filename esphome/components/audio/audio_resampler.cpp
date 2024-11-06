#ifdef USE_ESP_IDF

#include "audio_resampler.h"

#include "esphome/core/ring_buffer.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace audio {

static const size_t NUM_TAPS = 32;
static const size_t NUM_FILTERS = 32;
static const bool USE_PRE_POST_FILTER = true;

// These output parameters are currently hardcoded in the elements further down the pipeline (mixer and speaker)
static const uint8_t OUTPUT_CHANNELS = 2;
static const uint8_t OUTPUT_BYTES_PER_SAMPLE = sizeof(int16_t);
static const uint8_t OUTPUT_BITS_PER_SAMPLE = 8 * OUTPUT_BYTES_PER_SAMPLE;

static const size_t READ_WRITE_TIMEOUT_MS = 20;

AudioResampler::~AudioResampler() {
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);

  if (this->float_input_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_input_buffer_,
                               this->input_transfer_buffer_->capacity() / OUTPUT_BYTES_PER_SAMPLE);
  }
  if (this->float_output_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_output_buffer_,
                               this->output_transfer_buffer_->capacity() / OUTPUT_BYTES_PER_SAMPLE);
  }
  if (this->resampler_ != nullptr) {
    resampleFree(this->resampler_);
    this->resampler_ = nullptr;
  }
}

esp_err_t AudioResampler::allocate_buffers_(bool allocate_float_buffers) {
  if ((this->input_transfer_buffer_ == nullptr) || (this->output_transfer_buffer_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  if (allocate_float_buffers) {
    ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
    if (this->float_input_buffer_ == nullptr) {
      this->float_input_buffer_ = float_allocator.allocate(this->input_buffer_size_ / OUTPUT_BYTES_PER_SAMPLE);
    }

    if (this->float_output_buffer_ == nullptr) {
      this->float_output_buffer_ = float_allocator.allocate(this->output_buffer_size_ / OUTPUT_BYTES_PER_SAMPLE);
    }

    if ((this->float_input_buffer_ == nullptr) || (this->float_output_buffer_ == nullptr)) {
      return ESP_ERR_NO_MEM;
    } else {
      this->float_input_buffer_current_ = this->float_input_buffer_;
      this->float_input_buffer_length_ = 0;
      this->float_output_buffer_current_ = this->float_output_buffer_;
      this->float_output_buffer_length_ = 0;
    }
  }

  return ESP_OK;
}

esp_err_t AudioResampler::start(AudioStreamInfo &stream_info, uint32_t target_sample_rate,
                                ResampleInfo &resample_info) {
  this->stream_info_ = stream_info;

  resample_info.mono_to_stereo = (stream_info.channels != 2);

  if ((stream_info.channels > OUTPUT_CHANNELS) || (stream_info_.bits_per_sample != OUTPUT_BITS_PER_SAMPLE)) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (stream_info.channels > 0) {
    this->channel_factor_ = 2 / stream_info.channels;
  }

  if (stream_info.sample_rate != target_sample_rate) {
    int flags = 0;

    resample_info.resample = true;

    this->sample_ratio_ = static_cast<float>(target_sample_rate) / static_cast<float>(stream_info.sample_rate);

    if (this->sample_ratio_ < 1.0) {
      this->lowpass_ratio_ -= (10.24 / 16);

      if (this->lowpass_ratio_ < 0.84) {
        this->lowpass_ratio_ = 0.84;
      }

      if (this->lowpass_ratio_ < this->sample_ratio_) {
        // avoid discontinuities near unity sample ratios
        this->lowpass_ratio_ = this->sample_ratio_;
      }
    }
    if (this->lowpass_ratio_ * this->sample_ratio_ < 0.98 && USE_PRE_POST_FILTER) {
      float cutoff = this->lowpass_ratio_ * this->sample_ratio_ / 2.0;
      biquad_lowpass(&this->lowpass_coeff_, cutoff);
      this->pre_filter_ = true;
    }

    if (this->lowpass_ratio_ / this->sample_ratio_ < 0.98 && USE_PRE_POST_FILTER && !this->pre_filter_) {
      float cutoff = this->lowpass_ratio_ / this->sample_ratio_ / 2.0;
      biquad_lowpass(&this->lowpass_coeff_, cutoff);
      this->post_filter_ = true;
    }

    if (this->pre_filter_ || this->post_filter_) {
      for (int i = 0; i < stream_info.channels; ++i) {
        biquad_init(&this->lowpass_[i][0], &this->lowpass_coeff_, 1.0);
        biquad_init(&this->lowpass_[i][1], &this->lowpass_coeff_, 1.0);
      }
    }

    if (this->sample_ratio_ < 1.0) {
      this->resampler_ = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS,
                                      this->sample_ratio_ * this->lowpass_ratio_, flags | INCLUDE_LOWPASS);
    } else if (this->lowpass_ratio_ < 1.0) {
      this->resampler_ =
          resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, this->lowpass_ratio_, flags | INCLUDE_LOWPASS);
    } else {
      this->resampler_ = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, 1.0, flags);
    }

    resampleAdvancePosition(this->resampler_, NUM_TAPS / 2.0);

  } else {
    resample_info.resample = false;
  }

  this->resample_info_ = resample_info;

  esp_err_t err = this->allocate_buffers_(resample_info.resample);
  if (err != ESP_OK) {
    return err;
  }

  return ESP_OK;
}

AudioResamplerState AudioResampler::resample(bool stop_gracefully) {
  if (stop_gracefully) {
    if (!this->input_transfer_buffer_->has_buffered_data() && !this->output_transfer_buffer_->has_buffered_data()) {
      return AudioResamplerState::FINISHED;
    }
  }

  if (this->output_transfer_buffer_->available() > 0) {
    // this->output_transfer_buffer_->write_ring_buffer(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    this->output_transfer_buffer_->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    return AudioResamplerState::RESAMPLING;
  }

  // Refill the input buffer
  this->input_transfer_buffer_->transfer_data_from_source(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

  if (this->input_transfer_buffer_->available() == 0) {
    return AudioResamplerState::RESAMPLING;
  }

  /*
   Samples are indiviudal int16 values. Frames include 1 sample for mono and 2 samples for stereo
   Be careful converting between bytes, samples, and frames!
   1 sample = 2 bytes = sizeof(int16_t) = OUTPUT_BYTES_PER_SAMPLE
   if mono:
      1 frame = 1 sample
   if stereo:
      1 frame = 2 samples (left and right)
  */

  const size_t samples_free_to_write = this->output_transfer_buffer_->free() / OUTPUT_BYTES_PER_SAMPLE;
  const size_t frames_free_to_write = samples_free_to_write / OUTPUT_CHANNELS;

  size_t frames_to_read = frames_free_to_write;
  if (this->sample_ratio_ > 1.0) {
    // Upsampling, so we have to read less frames
    uint32_t upsampling_factor = std::ceil(this->sample_ratio_);
    frames_to_read /= upsampling_factor;
  }

  const size_t samples_to_read = frames_to_read * this->stream_info_.channels;

  size_t samples_to_process =
      std::min(samples_to_read, this->input_transfer_buffer_->available() / OUTPUT_BYTES_PER_SAMPLE);

  size_t bytes_to_transfer = samples_to_process * OUTPUT_BYTES_PER_SAMPLE;

  // Pointer to where new output data is going to be written
  uint8_t *new_output_data = this->output_transfer_buffer_->get_buffer_end();

  if (this->resample_info_.resample) {
    for (size_t i = 0; i < samples_to_process; ++i) {
      this->float_input_buffer_[i] =
          static_cast<float>(reinterpret_cast<int16_t *>(this->input_transfer_buffer_->get_buffer_start())[i]) /
          32768.0f;
    }
    size_t frames_read = samples_to_process / this->stream_info_.channels;

    if (this->pre_filter_) {
      for (int i = 0; i < this->stream_info_.channels; ++i) {
        biquad_apply_buffer(&this->lowpass_[i][0], this->float_input_buffer_ + i, frames_read,
                            this->stream_info_.channels);
        biquad_apply_buffer(&this->lowpass_[i][1], this->float_input_buffer_ + i, frames_read,
                            this->stream_info_.channels);
      }
    }

    ResampleResult res;

    res = resampleProcessInterleaved(
        this->resampler_, this->float_input_buffer_, frames_read, this->float_output_buffer_,
        (this->output_transfer_buffer_->capacity() / OUTPUT_BYTES_PER_SAMPLE) / this->channel_factor_,
        this->sample_ratio_);

    size_t frames_used = res.input_used;
    size_t samples_used = frames_used * this->stream_info_.channels;

    size_t frames_generated = res.output_generated;
    if (this->post_filter_) {
      for (int i = 0; i < this->stream_info_.channels; ++i) {
        biquad_apply_buffer(&this->lowpass_[i][0], this->float_output_buffer_ + i, frames_generated,
                            this->stream_info_.channels);
        biquad_apply_buffer(&this->lowpass_[i][1], this->float_output_buffer_ + i, frames_generated,
                            this->stream_info_.channels);
      }
    }

    size_t samples_generated = frames_generated * this->stream_info_.channels;

    for (size_t i = 0; i < samples_generated; ++i) {
      reinterpret_cast<int16_t *>(new_output_data)[i] = static_cast<int16_t>(this->float_output_buffer_[i] * 32767);
    }

    this->input_transfer_buffer_->decrease_buffer_length(samples_used * OUTPUT_BYTES_PER_SAMPLE);
    bytes_to_transfer = samples_generated * OUTPUT_BYTES_PER_SAMPLE;
  } else {
    // No resampling required, copy int16 samples to output transfer buffer
    std::memcpy((void *) new_output_data, (void *) this->input_transfer_buffer_->get_buffer_start(), bytes_to_transfer);
    this->input_transfer_buffer_->decrease_buffer_length(bytes_to_transfer);
  }

  if (this->resample_info_.mono_to_stereo) {
    // Convert mono to stereo in place
    size_t samples_to_duplicate = bytes_to_transfer / OUTPUT_BYTES_PER_SAMPLE;
    for (int i = (int) samples_to_duplicate - 1; i >= 0; --i) {
      reinterpret_cast<int16_t *>(new_output_data)[2 * i] = reinterpret_cast<int16_t *>(new_output_data)[i];
      reinterpret_cast<int16_t *>(new_output_data)[2 * i + 1] = reinterpret_cast<int16_t *>(new_output_data)[i];
    }

    this->output_transfer_buffer_->increase_buffer_length(2 *
                                                          bytes_to_transfer);  // double the bytes for stereo samples
  } else {
    this->output_transfer_buffer_->increase_buffer_length(bytes_to_transfer);
  }

  return AudioResamplerState::RESAMPLING;
}

}  // namespace audio
}  // namespace esphome

#endif
