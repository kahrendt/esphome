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

    double b0, b1, b2, a0, a1, a2;  // BiQuad coefficients
    double A, W0, S, C, alpha;      // Intermediate calculation values

    W0 = (_TWO_PI * frequency) / _FS;
    S = sin(W0);
    C = cos(W0);
    alpha = S / (2 * q);
    A = pow(10, gain / 40.0);

    b0 = 1 + alpha * A;
    b1 = -2 * cos(W0);
    b2 = 1 - alpha * A;
    a0 = 1 + alpha / A;
    a1 = 2 * cos(W0);
    a2 = -(1 - alpha / A);

    // Normalize the BiQuad values
    a1 /= a0;
    a2 /= a0;
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;

    // Return filter BiQuad values (
    coeffs[0] = b0;
    coeffs[1] = b1;
    coeffs[2] = b2;
    coeffs[3] = a1;
    coeffs[4] = a2;

    history[0] = 0.0f;
    history[1] = 0.0f;
  }
  float coeffs[6];
  float history[2];
};

class Equalizer {
 public:
  Equalizer(size_t processing_frames) : processing_frames_(processing_frames) {}
  ~Equalizer();

  bool initialize(uint8_t channels);

  void equalize(const int16_t *input, int16_t *output, size_t frames_to_process, uint32_t &clipped_samples);

  void add_peak_eq(double frequency, double q, double gain) {
    filter_biquad new_filter;
    new_filter.set_peak_eq(frequency, q, gain);
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
