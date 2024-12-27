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

struct filter_biquad {
  void set_peak_eq(double frequency, double q, double gain) {
    // based on https://github.com/steindevices/ESP32-LyraT-DSP/
    // https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

    double b0, b1, b2, a0, a1, a2;  // BiQuad coefficients
    double amp, w0, s, c, alpha;    // Intermediate calculation values

    w0 = (_TWO_PI * frequency) / _FS;
    s = sin(w0);
    c = cos(w0);
    alpha = s / (2 * q);
    amp = pow(10, gain / 40.0);

    b0 = 1 + alpha * amp;
    b1 = -2 * c;
    b2 = 1 - alpha * amp;
    a0 = 1 + alpha / amp;
    a1 = -(2 * c);
    a2 = (1 - alpha / amp);

    // Normalize the BiQuad values
    a1 /= a0;
    a2 /= a0;
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;

    // Return filter BiQuad values (
    coeffs[0] = b0 / a0;
    coeffs[1] = b1 / a0;
    coeffs[2] = b2 / a0;
    coeffs[3] = a1 / a0;
    coeffs[4] = a2 / a0;

    printf("coefficeints %.3f,%.3f,%.3f,%.3f,%.3f\n", b0, b1, b2, a1, a2);
  }
  void set_lpf(double frequency, double q) {
    // based on https://github.com/steindevices/ESP32-LyraT-DSP/
    // https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

    double b0, b1, b2, a0, a1, a2;  // BiQuad coefficients
    double amp, w0, s, c, alpha;    // Intermediate calculation values

    w0 = (_TWO_PI * frequency) / _FS;
    s = sin(w0);
    c = cos(w0);
    alpha = s / (2 * q);

    b0 = (1 - c) / 2;
    b1 = 1 - c;
    b2 = (1 - c) / 2;
    a0 = 1 + alpha;
    a1 = -2.0 * c;
    a2 = 1.0f - alpha;

    // Normalize the BiQuad values
    a1 /= a0;
    a2 /= a0;
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;

    // Return filter BiQuad values (
    coeffs[0] = b0 / a0;
    coeffs[1] = b1 / a0;
    coeffs[2] = b2 / a0;
    coeffs[3] = a1 / a0;
    coeffs[4] = a2 / a0;

    printf("coefficeints %.3f,%.3f,%.3f,%.3f,%.3f\n", b0, b1, b2, a1, a2);
  }

  float coeffs[5];
  float history[2] = {0.0f, 0.0f};
};

class Equalizer {
 public:
  Equalizer(size_t processing_frames) : processing_frames_(processing_frames) {}
  ~Equalizer();

  bool initialize(uint8_t channels);

  void equalize(const int16_t *input, int16_t *output, size_t frames_to_process, uint32_t &clipped_samples);

  void add_peak_eq(uint8_t channel, double frequency, double q, double gain) {
    filter_biquad new_filter;

    std::vector<filter_biquad> filters;
    if (this->channel_filters_.size() > channel) {
      filters = this->channel_filters_[channel];
    } else {
      this->channel_filters_.push_back(filters);
    }

    new_filter.set_peak_eq(frequency, q, gain);
    filters.push_back(new_filter);
  }
  void add_lpf(uint8_t channel, double frequency, double q) {
    filter_biquad new_filter;

    std::vector<filter_biquad> filters;
    if (this->channel_filters_.size() <= channel) {
      filters = this->channel_filters_[channel];
    } else {
      this->channel_filters_.push_back(filters);
    }

    new_filter.set_lpf(frequency, q);
    filters.push_back(new_filter);
  }

 protected:
  void tpdf_dither_init_(int num_channels);
  float tpdf_dither_(int channel, int type);

  std::vector<std::vector<filter_biquad>> channel_filters_;

  size_t processing_frames_;

  float **float_buffers_{nullptr};

  uint8_t channels_;

  uint32_t *tpdf_generators_{nullptr};
  float *error_{nullptr};
};
}  // namespace equalizer
