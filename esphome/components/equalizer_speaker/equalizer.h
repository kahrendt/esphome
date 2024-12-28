#pragma once

#include <math.h>
#include <esp_heap_caps.h>
#include <vector>

namespace equalizer {

#define _PI 3.14159265358979323846  /* pi    */
#define _LN2 0.69314718055994530942 /* ln(2) */
#define _LN_CONST (_LN2 / 2)        /* ln(2)/2 */
#define _TWO_PI (_PI * 2)           /* 2*pi */
#define _FS 48000                   /* sampling frequency */

enum class EqualizerFilters {
  LOW_PASS_FILTER,
  HIGH_PASS_FILTER,
  BAND_PASS_FILTER,
  NOTCH_FILTER,
  ACTIVE_POWER_FILTER,
  PEAKING_EQ_FILTER,
  LOW_SHELF_FILTER,
  HIGH_SHELF_FILTER,
};

struct filter_biquad {
  void set_filter_coeffecients(uint8_t channel, EqualizerFilters type, double frequency, double q, double gain) {
    // Formulas based on https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

    this->channel = channel;

    double b0, b1, b2, a0, a1, a2;  // BiQuad coefficients
    double amp, w0, s, c, alpha;    // Intermediate calculation values

    w0 = (_TWO_PI * frequency) / _FS;
    s = sin(w0);
    c = cos(w0);
    alpha = s / (2 * q);
    amp = pow(10, gain / 40.0);

    switch (type) {
      case EqualizerFilters::LOW_PASS_FILTER:
        b0 = (1 - c) / 2;
        b1 = 1 - c;
        b2 = (1 - c) / 2;
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        break;
      case EqualizerFilters::HIGH_PASS_FILTER:
        b0 = (1 + c) / 2;
        b1 = -(1 + c);
        b2 = (1 + c) / 2;
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        break;
      case EqualizerFilters::BAND_PASS_FILTER:
        // Constant 0 dB peak gain version
        b0 = alpha;
        b1 = 0;
        b2 = -alpha;
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        break;
      case EqualizerFilters::NOTCH_FILTER:
        b0 = 1;
        b1 = -2 * c;
        b2 = 1;
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        break;
      case EqualizerFilters::ACTIVE_POWER_FILTER:
        b0 = 1 - alpha;
        b1 = -2 * c;
        b2 = 1 + alpha;
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        break;
      case EqualizerFilters::PEAKING_EQ_FILTER:
        b0 = 1 + alpha * amp;
        b1 = -2 * c;
        b2 = 1 - alpha * amp;
        a0 = 1 + alpha / amp;
        a1 = -(2 * c);
        a2 = (1 - alpha / amp);
        break;
      case EqualizerFilters::LOW_SHELF_FILTER:
        // clang-format off
        b0 =      amp * ((amp + 1) - (amp - 1) * c + 2 * sqrt(amp) * alpha);
        b1 =  2 * amp * ((amp - 1) - (amp + 1) * c);
        b2 =      amp * ((amp + 1) - (amp - 1) * c - 2 * sqrt(amp) * alpha);
        a0 =            ((amp + 1) + (amp - 1) * c + 2 * sqrt(amp) * alpha);
        a1 = -2 *       ((amp - 1) + (amp + 1) * c);
        a2 =            ((amp + 1) + (amp - 1) * c - 2 * sqrt(amp) * alpha);
        break;
      case EqualizerFilters::HIGH_SHELF_FILTER:
        b0 =      amp * ((amp + 1) + (amp - 1) * c + 2 * sqrt(amp) * alpha);
        b1 = -2 * amp * ((amp - 1) + (amp + 1) * c);
        b2 =      amp * ((amp + 1) + (amp - 1) * c - 2 * sqrt(amp) * alpha);
        a0 =            ((amp + 1) - (amp - 1) * c + 2 * sqrt(amp) * alpha);
        a1 =  2 *       ((amp - 1) - (amp + 1) * c);
        a2 =            ((amp + 1) - (amp - 1) * c - 2 * sqrt(amp) * alpha);
        // clang-format on
        break;
    }

    // Use normalized filter coeffecients
    this->coeffs[0] = b0 / a0;
    this->coeffs[1] = b1 / a0;
    this->coeffs[2] = b2 / a0;
    this->coeffs[3] = a1 / a0;
    this->coeffs[4] = a2 / a0;
  }

  uint8_t channel;
  float coeffs[5];
  float history[2] = {0.0f, 0.0f};
};

class Equalizer {
 public:
  Equalizer(size_t processing_frames) : processing_frames_(processing_frames) {}
  ~Equalizer();

  bool initialize(uint8_t channels);

  void equalize(const int16_t *input_buffer, uint8_t *output_buffer, size_t frames_to_process,
                uint32_t &clipped_samples);

  void add_filter(uint8_t channel, EqualizerFilters type, double frequency, double q, double gain) {
    filter_biquad new_filter;

    new_filter.set_filter_coeffecients(channel, type, frequency, q, gain);
    this->filters_.push_back(new_filter);
  }

 protected:
  void tpdf_dither_init_(int num_channels);
  float tpdf_dither_(int channel, int type);

  std::vector<filter_biquad> filters_;

  size_t processing_frames_;

  float **float_buffers_{nullptr};

  uint8_t channels_;

  uint32_t *tpdf_generators_{nullptr};
  float *error_{nullptr};
};
}  // namespace equalizer
