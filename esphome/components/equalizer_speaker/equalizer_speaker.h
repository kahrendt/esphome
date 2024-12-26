#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace equalizer_speaker {

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

    W0 = (_TWO_PI * filter->frequency) / _FS;
    S = sin(W0);
    C = cos(W0);
    alpha = S / (2 * filter->Q);
    A = pow(10, filter->gain / 40.0);

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
  }
  float coeffs[6];
};

class EqualizerSpeaker : public Component, public speaker::Speaker {
 public:
  void setup() override;
  void loop() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  void set_pause_state(bool pause_state) override { this->output_speaker_->set_pause_state(pause_state); }
  bool get_pause_state() const override { return this->output_speaker_->get_pause_state(); }

  bool has_buffered_data() const override;

  /// @brief Mute state changes are passed to the parent's output speaker
  void set_mute_state(bool mute_state) override;

  /// @brief Volume state changes are passed to the parent's output speaker
  void set_volume(float volume) override;

  void set_target_sample_rate(uint32_t target_sample_rate) { this->target_sample_rate_ = target_sample_rate; }
  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }

  void add_peak_eq(double frequency, double q, double gain) {
    filter_biquad new_filter;
    new_filter.set_peak_eq(frequency, q, gain);
    this->filters_.push_back(new_filter);
  }

 protected:
  esp_err_t start_();
  void stop_();

  static void equalizer_task(void *params);

  std::vector<filter_biquad> filters_;

  TaskHandle_t task_handle_{nullptr};
  EventGroupHandle_t event_group_{nullptr};

  std::weak_ptr<RingBuffer> ring_buffer_;

  speaker::Speaker *output_speaker_{nullptr};

  bool task_created_{false};

  uint32_t target_sample_rate_;
};

}  // namespace equalizer_speaker
}  // namespace esphome

#endif
