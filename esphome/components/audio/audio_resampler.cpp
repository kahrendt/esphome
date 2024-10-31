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
static const uint8_t OUTPUT_BITS_PER_SAMPLE = 16;

static const size_t READ_WRITE_TIMEOUT_MS = 20;

AudioResampler::~AudioResampler() {
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);

  if (this->float_input_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_input_buffer_, this->internal_buffer_samples_);
  }
  if (this->float_output_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_output_buffer_, this->internal_buffer_samples_);
  }
  if (this->resampler_ != nullptr) {
    resampleFree(this->resampler_);
    this->resampler_ = nullptr;
  }
}

esp_err_t AudioResampler::allocate_buffers_() {
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);

  if (!this->input_transfer_buffer_->allocated_successfully() ||
      !this->output_transfer_buffer_->allocated_successfully()) {
    return ESP_ERR_NO_MEM;
  }

  if (this->float_input_buffer_ == nullptr)
    this->float_input_buffer_ = float_allocator.allocate(this->internal_buffer_samples_);

  if (this->float_output_buffer_ == nullptr)
    this->float_output_buffer_ = float_allocator.allocate(this->internal_buffer_samples_);

  if ((this->float_input_buffer_ == nullptr) || (this->float_output_buffer_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t AudioResampler::start(AudioStreamInfo &stream_info, uint32_t target_sample_rate,
                                ResampleInfo &resample_info) {
  esp_err_t err = this->allocate_buffers_();
  if (err != ESP_OK) {
    return err;
  }

  this->stream_info_ = stream_info;

  this->float_input_buffer_current_ = this->float_input_buffer_;
  this->float_input_buffer_length_ = 0;

  this->float_output_buffer_current_ = this->float_output_buffer_;
  this->float_output_buffer_length_ = 0;

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
  return ESP_OK;
}

AudioResamplerState AudioResampler::resample(bool stop_gracefully) {
  if (stop_gracefully) {
    if ((this->input_transfer_buffer_->get_ring_buffer()->available() == 0) &&
        (this->output_transfer_buffer_->get_ring_buffer()->available() == 0) &&
        (this->input_transfer_buffer_->available() == 0) && (this->output_transfer_buffer_->available() == 0)) {
      return AudioResamplerState::FINISHED;
    }
  }

  if (this->output_transfer_buffer_->available() > 0) {
    this->output_transfer_buffer_->write_ring_buffer(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    return AudioResamplerState::RESAMPLING;
  }

  // Refill the input buffer
  this->input_transfer_buffer_->read_ring_buffer(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

  if (this->input_transfer_buffer_->available() == 0) {
    return AudioResamplerState::RESAMPLING;
  }

  // Depending on if we are converting mono to stereo or if we are upsampling, we may need to restrict how many samples
  // we process
  size_t max_samples_to_process = this->input_transfer_buffer_->available() / sizeof(int16_t);

  // Mono to stereo -> cut in half
  max_samples_to_process /= (2 / this->stream_info_.channels);

  if (this->sample_ratio_ > 1.0) {
    // Upsampling -> reduce by a factor of the ceiling of sample_ratio_
    uint32_t upsampling_factor = std::ceil(this->sample_ratio_);
    max_samples_to_process /= upsampling_factor;
  }

  // We can't process more samples than we can store
  max_samples_to_process = std::min(max_samples_to_process, this->output_transfer_buffer_->free() / sizeof(int16_t));

  size_t bytes_to_transfer = max_samples_to_process * sizeof(int16_t);

  // Pointer to where new output data is going to be written
  uint8_t *new_output_data = this->output_transfer_buffer_->get_buffer_end();

  if (this->resample_info_.resample) {
    //   if (this->input_buffer_length_ > 0) {
    //     // Samples are indiviudal int16 values. Frames include 1 sample for mono and 2 samples for stereo
    //     // Be careful converting between bytes, samples, and frames!
    //     // 1 sample = 2 bytes = sizeof(int16_t)
    //     // if mono:
    //     //    1 frame = 1 sample
    //     // if stereo:
    //     //    1 frame = 2 samples (left and right)

    //     size_t samples_read = this->input_buffer_length_ / sizeof(int16_t);

    //     for (size_t i = 0; i < samples_read; ++i) {
    //       this->float_input_buffer_[i] = static_cast<float>(this->input_buffer_[i]) / 32768.0f;
    //     }

    //     size_t frames_read = samples_read / this->stream_info_.channels;

    //     if (this->pre_filter_) {
    //       for (int i = 0; i < this->stream_info_.channels; ++i) {
    //         biquad_apply_buffer(&this->lowpass_[i][0], this->float_input_buffer_ + i, frames_read,
    //                             this->stream_info_.channels);
    //         biquad_apply_buffer(&this->lowpass_[i][1], this->float_input_buffer_ + i, frames_read,
    //                             this->stream_info_.channels);
    //       }
    //     }

    //     ResampleResult res;

    //     res = resampleProcessInterleaved(this->resampler_, this->float_input_buffer_, frames_read,
    //                                      this->float_output_buffer_,
    //                                      this->internal_buffer_samples_ / this->channel_factor_,
    //                                      this->sample_ratio_);

    //     size_t frames_used = res.input_used;
    //     size_t samples_used = frames_used * this->stream_info_.channels;

    //     size_t frames_generated = res.output_generated;
    //     if (this->post_filter_) {
    //       for (int i = 0; i < this->stream_info_.channels; ++i) {
    //         biquad_apply_buffer(&this->lowpass_[i][0], this->float_output_buffer_ + i, frames_generated,
    //                             this->stream_info_.channels);
    //         biquad_apply_buffer(&this->lowpass_[i][1], this->float_output_buffer_ + i, frames_generated,
    //                             this->stream_info_.channels);
    //       }
    //     }

    //     size_t samples_generated = frames_generated * this->stream_info_.channels;

    // for (size_t i = 0; i < samples_generated; ++i) {
    //   this->output_buffer_[i] = static_cast<int16_t>(this->float_output_buffer_[i] * 32767);
    // }

    // this->input_buffer_current_ += samples_used;
    // this->input_buffer_length_ -= samples_used * sizeof(int16_t);

    // this->output_buffer_current_ = this->output_buffer_;
    // this->output_buffer_length_ += samples_generated * sizeof(int16_t);
    // }
  } else {
    // No resampling required, direclty copy to output transfer buffer
    std::memcpy((void *) new_output_data, (void *) this->input_transfer_buffer_->get_buffer_start(), bytes_to_transfer);
    this->input_transfer_buffer_->decrease_buffer_length(bytes_to_transfer);
  }

  if (this->resample_info_.mono_to_stereo) {
    // Convert mono to stereo
    size_t samples_to_duplicate = bytes_to_transfer / sizeof(int16_t);
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
