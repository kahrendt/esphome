#include "equalizer.h"
#include "dsps_biquad.h"

#include "esp_random.h"

#include <cstring>

namespace equalizer {

// static void dsps_biquad_f32_ansi(const float *input, float *output, int len, float *coef, float *w) {
//   for (int i = 0; i < len; i++) {
//     float d0 = input[i] - coef[3] * w[0] - coef[4] * w[1];
//     output[i] = coef[0] * d0 + coef[1] * w[0] + coef[2] * w[1];
//     w[1] = w[0];
//     w[0] = d0;
//   }
// }

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

  // // Bookshelf speakers
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 78, 5.7, -4.5);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 127.4, 7.9, -5.4);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 207.7, 8.1, -3.1);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 309.6, 4.6, -4.9);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 501.4, 5.1, 1.5);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 845.2, 7.0, -4.3);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 1129, 9.2, 2.9);
  // this->add_filter(0, EqualizerFilters::HIGH_SHELF_FILTER, 1464.3, 0.5, -10.0);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 1995.6, 6.7, -2.3);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 3637.3, 4.4, 1.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 78, 5.7, -4.5);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 127.4, 7.9, -5.4);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 207.7, 8.1, -3.1);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 309.6, 4.6, -4.9);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 501.4, 5.1, 1.5);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 845.2, 7.0, -4.3);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 1129, 9.2, 2.9);
  // this->add_filter(1, EqualizerFilters::HIGH_SHELF_FILTER, 1464.3, 0.5, -10.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 1995.6, 6.7, -2.3);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 3637.3, 4.4, 1.0);

  // Internal speaker
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 415, 5.8, 2.6);
  this->add_filter(0, EqualizerFilters::HIGH_SHELF_FILTER, 504.2, 1.0, -10);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 651.8, 4.0, -5.6);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 947.1, 8.0, 2.1);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 1260.0, 7.9, -1.5);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 1834.0, 7.9, 3.1);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 2231.0, 10.0, -2.8);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 2691.7, 1.7, -10.0);
  this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 2954.0, 9.1, -4.4);
  this->add_filter(0, EqualizerFilters::HIGH_SHELF_FILTER, 3517.0, 1.0, 1.5);

  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 189.9, 5.3, 10.0);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 251.9, 1.5, 10.0);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 306.7, 1.3, 10.0);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 403.5, 4.3, 7.1);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 129.5, 1.5, -3.3);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 153.2, 7.1, -4.8);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 1073.6, 5.0, -4.2);
  // this->add_filter(0, EqualizerFilters::HIGH_SHELF_FILTER, 1831.9, 1.0, -6.5);

  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 644.0, 4.1, -7.9);
  // this->add_filter(0, EqualizerFilters::PEAKING_EQ_FILTER, 2724.2, 2.6, -8.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 129.5, 1.5, -3.3);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 153.2, 7.1, -4.8);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 189.9, 5.3, 10.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 251.9, 1.5, 10.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 306.7, 1.3, 10.0);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 403.5, 4.3, 7.1);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 644.0, 4.1, -7.9);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 1073.6, 5.0, -4.2);
  // this->add_filter(1, EqualizerFilters::HIGH_SHELF_FILTER, 1831.9, 1.0, -6.5);
  // this->add_filter(1, EqualizerFilters::PEAKING_EQ_FILTER, 2724.2, 2.6, -8.0);

  return true;
}

void Equalizer::equalize(const int16_t *input_buffer, uint8_t *output_buffer, size_t frames_to_process,
                         uint32_t &clipped_samples) {
  // Convert fixed point samples to floating point
  for (unsigned int frame = 0; frame < frames_to_process; ++frame) {
    for (uint8_t channel = 0; channel < this->channels_; ++channel) {
      this->float_buffers_[channel][frame] =
          (static_cast<float>(input_buffer[this->channels_ * frame + channel]) / 32768.0f);
      //   *0.12;
    }
  }
  // // printf("flloat buffer 0 %.5f\n", this->float_buffers_[0][0]);

  for (auto &filter : this->filters_) {
    dsps_biquad_f32(float_buffers_[filter.channel], float_buffers_[filter.channel], frames_to_process, filter.coeffs,
                    filter.history);
  }

  const uint8_t out_bits = 16;

  if (out_bits != 32) {
    float scaler = (1 << out_bits) / 2.0;
    int32_t offset = (out_bits <= 8) * 128;
    int32_t high_clip = (1 << (out_bits - 1)) - 1;
    int32_t low_clip = ~high_clip;
    int left_shift = (24 - out_bits) % 8;
    size_t i, j;
    clipped_samples = 0;

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
      output_buffer[j++] = output = (output << left_shift) + offset;
      if (out_bits > 8) {
        output_buffer[j++] = output >> 8;

        if (out_bits > 16) {
          output_buffer[j++] = output >> 16;
        }
      }
    }
  } else {
    // float scaler = (1 << out_bits) / 2.0;
    // int32_t offset = (out_bits <= 8) * 128;
    // int32_t high_clip = (1 << (out_bits - 1)) - 1;
    // int32_t low_clip = ~high_clip;
    // int left_shift = (24 - out_bits) % 8;
    // size_t i, j;
    // clipped_samples = 0;

    // for (i = j = 0; i < frames_to_process * this->channels_; ++i) {
    //   uint8_t chan = i % this->channels_;
    //   float float_sample = this->float_buffers_[chan][i / this->channels_];
    //   int32_t output = float_sample * scalar;
    //   if (float_sample >= 1.0f) {
    //     ++clipped_samples;
    //     output = std::numeric_limits<std::int32_t>::max();
    //   } else if (float_sample < 1.0f) {
    //     ++clipped_samples;
    //     output = std::numeric_limits<std::int32_t>::min();
    //   }

    //   // output_buffer[j++] = output;
    //   // output_buffer[j++] =
    //   // output = (output << left_shift) + offset;
    //   // output_buffer[j++] = output = (output << left_shift) + offset;
    //   // if (out_bits > 8) {
    //   //   output_buffer[j++] = output >> 8;

    //   //   if (out_bits > 16) {
    //   //     output_buffer[j++] = output >> 16;
    //   //   }
    //   // }
    // }
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
