#include "equalizer.h"

#include "esp_random.h"

#include <cstring>

namespace equalizer {

static void dsps_biquad_f32_ansi(const float *input, float *output, int len, float *coef, float *w) {
  for (int i = 0; i < len; i++) {
    float d0 = input[i] - coef[3] * w[0] - coef[4] * w[1];
    output[i] = coef[0] * d0 + coef[1] * w[0] + coef[2] * w[1];
    w[1] = w[0];
    w[0] = d0;
  }
}

Equalizer::~Equalizer() {
  if (this->float_buffers_ != nullptr) {
    for (uint8_t i = 0; i < this->channels_; ++i) {
      if (this->float_buffers_[i] != nullptr) {
        free(this->float_buffers_[i]);
      }
    }
    free(this->float_buffers_);
  }

  if (this->tpdf_generators_ != nullptr) {
    free(this->tpdf_generators_);
  }
};

bool Equalizer::initialize(uint8_t channels) {
  this->channels_ = channels;

  this->float_buffers_ = (float **) calloc(channels, sizeof(float *));

  if (this->float_buffers_ == nullptr) {
    return false;
  }

  for (uint8_t i = 0; i < channels; ++i) {
    this->float_buffers_[i] =
        (float *) heap_caps_malloc(this->processing_frames_ * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (this->float_buffers_[i] == nullptr) {
      return false;
    }
  }

  this->error_ = (float *) malloc(channels * sizeof(float));
  std::memset(this->error_, 0, channels * sizeof(float));
  this->tpdf_dither_init_(channels);

  return true;
}

void Equalizer::equalize(const int16_t *input_buffer, int16_t *output_buffer, size_t frames_to_process,
                         uint32_t &clipped_samples) {
  // Convert fixed point samples to floating point
  for (unsigned int frame = 0; frame < frames_to_process; ++frame) {
    for (uint8_t channel = 0; channel < this->channels_; ++channel) {
      this->float_buffers_[channel][frame] =
          static_cast<float>(input_buffer[this->channels_ * frame + channel]) / 32768.0f;
    }
  }

  for (uint8_t channel = 0; channel < this->channels_; ++channel) {
    // Need a separate set of filters for each channel!
    for (auto &filter : this->filters_) {
      dsps_biquad_f32_ansi(float_buffers_[channel], float_buffers_[channel], frames_to_process, filter.coeffs,
                           filter.history);
    }
  }

  const size_t samples_generated = frames_to_process * this->channels_;

  const uint8_t out_bits = 16;

  float scaler = (1 << out_bits) / 2.0;
  int32_t offset = (out_bits <= 8) * 128;
  int32_t high_clip = (1 << (out_bits - 1)) - 1;
  int32_t low_clip = ~high_clip;
  clipped_samples = 0;

  for (unsigned int frame = 0; frame < frames_to_process; ++frame) {
    for (uint8_t channel = 0; channel < this->channels_; ++channel) {
      int32_t output = floor((this->float_buffers_[channel][frame] *= scaler) - this->error_[channel] +
                             this->tpdf_dither_(channel, -1) + 0.5);
      if (output > high_clip) {
        ++clipped_samples;
        output = high_clip;
      } else if (output < low_clip) {
        ++clipped_samples;
        output = low_clip;
      }

      this->error_[channel] += output - this->float_buffers_[channel][frame];
      output_buffer[this->channels_ * frame + channel] = (output >> 16);
    }
  }
}

void Equalizer::tpdf_dither_init_(int num_channels) {
  this->tpdf_generators_ = (uint32_t *) malloc(num_channels * sizeof(uint32_t));

  for (size_t i = 0; i < num_channels; ++i) {
    this->tpdf_generators_[i] = esp_random();
  }
}

float Equalizer::tpdf_dither_(int channel, int type) {
  uint32_t random = this->tpdf_generators_[channel];
  random = ((random << 4) - random) ^ 1;
  random = ((random << 4) - random) ^ 1;
  uint32_t first = type ? this->tpdf_generators_[channel] ^ ((int32_t) type >> 31) : ~random;
  random = ((random << 4) - random) ^ 1;
  random = ((random << 4) - random) ^ 1;
  random = ((random << 4) - random) ^ 1;
  this->tpdf_generators_[channel] = random;
  return (((first >> 1) + (random >> 1)) / 2147483648.0) - 1.0;
}

}  // namespace equalizer
