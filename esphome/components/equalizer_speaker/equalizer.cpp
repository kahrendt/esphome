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

  if (this->error_ != nullptr) {
    free(this->error_);
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

  // Bookshelf speakers
  add_peak_eq(0, 84.4, 1.6, -4.8);
  add_peak_eq(0, 103.7, 9.0, -6.5);
  add_peak_eq(0, 119.8, 9.0, 8.8);
  add_peak_eq(0, 139.2, 2.7, 10.0);
  add_peak_eq(0, 221.6, 1.0, 10.0);
  add_peak_eq(0, 243.8, 0.9, 10.0);
  add_peak_eq(0, 359.0, 2.5, 3.1);
  add_peak_eq(0, 698.5, 3.9, -7.4);
  add_peak_eq(0, 2870, 4.3, -7.4);
  add_peak_eq(0, 4000, 0.5, -10.0);
  add_peak_eq(1, 84.4, 1.6, -4.8);
  add_peak_eq(1, 103.7, 9.0, -6.5);
  add_peak_eq(1, 119.8, 9.0, 8.8);
  add_peak_eq(1, 139.2, 2.7, 10.0);
  add_peak_eq(1, 221.6, 1.0, 10.0);
  add_peak_eq(1, 243.8, 0.9, 10.0);
  add_peak_eq(1, 359.0, 2.5, 3.1);
  add_peak_eq(1, 698.5, 3.9, -7.4);
  add_peak_eq(1, 2870, 4.3, -7.4);
  add_peak_eq(1, 4000, 0.5, -10.0);

  // // Internal speaker
  // add_peak_eq(0, 82.9, 1.3, -3.8);
  // add_peak_eq(0, 101, 7.3, -6.9);
  // add_peak_eq(0, 113.7, 10.0, 9.0);
  // add_peak_eq(0, 121.7, 5.9, 10.0);
  // add_peak_eq(0, 171.5, 1.9, 7.2);
  // add_peak_eq(0, 239.7, 0.9, 10.0);
  // add_peak_eq(0, 241.5, 0.9, 10.0);
  // add_peak_eq(0, 696.9, 2.1, -5.0);
  // add_peak_eq(0, 3004.9, 2.8, -10.0);
  // add_peak_eq(0, 4000, 0.5, -10.0);
  // add_peak_eq(1, 82.9, 1.3, -3.8);
  // add_peak_eq(1, 101, 7.3, -6.9);
  // add_peak_eq(1, 113.7, 10.0, 9.0);
  // add_peak_eq(1, 121.7, 5.9, 10.0);
  // add_peak_eq(1, 171.5, 1.9, 7.2);
  // add_peak_eq(1, 239.7, 0.9, 10.0);
  // add_peak_eq(1, 241.5, 0.9, 10.0);
  // add_peak_eq(1, 696.9, 2.1, -5.0);
  // add_peak_eq(1, 3004.9, 2.8, -10.0);
  // add_peak_eq(1, 4000, 0.5, -10.0);

  // add_peak_eq(25.75, 2.027, -1.4);
  // add_peak_eq(52.4, 7.441, 1.4);
  // add_peak_eq(122.5, 18.708, -1.9);
  // add_peak_eq(139, 2.214, 10);
  // add_peak_eq(153.5, 2.001, -23.6);
  // add_lpf(400, 0.7);
  // add_peak_eq(4000, 5.0, 0.1);

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
  // // printf("flloat buffer 0 %.5f\n", this->float_buffers_[0][0]);
  for (uint8_t channel = 0; channel < this->channels_; ++channel) {
    // Need a separate set of filters for each channel!
    for (auto &filter : this->channel_filters_[channel]) {
      dsps_biquad_f32_ansi(float_buffers_[channel], float_buffers_[channel], frames_to_process, filter.coeffs,
                           filter.history);
    }
  }

  const uint8_t out_bits = 16;

  float scaler = (1 << out_bits) / 2.0;
  int32_t offset = (out_bits <= 8) * 128;
  int32_t high_clip = (1 << (out_bits - 1)) - 1;
  int32_t low_clip = ~high_clip;
  int left_shift = (24 - out_bits) % 8;
  size_t i, j;
  clipped_samples = 0;

  uint8_t *temp_buffer = (uint8_t *) (output_buffer);

  for (i = j = 0; i < frames_to_process * this->channels_; ++i) {
    uint8_t chan = i % this->channels_;
    int32_t output = floor((this->float_buffers_[chan][i / this->channels_] *= scaler) - this->error_[chan] +
                           this->tpdf_dither_(chan, -1) + 0.5);
    if (output > high_clip) {
      ++clipped_samples;
      output = high_clip;
    } else if (output < low_clip) {
      ++clipped_samples;
      output = low_clip;
    }

    this->error_[chan] += output - this->float_buffers_[chan][i / this->channels_];
    output = (output << left_shift) + offset;
    temp_buffer[j++] = output = (output << left_shift) + offset;
    if (out_bits > 8) {
      temp_buffer[j++] = output >> 8;

      if (out_bits > 16) {
        temp_buffer[j++] = output >> 16;
      }
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
