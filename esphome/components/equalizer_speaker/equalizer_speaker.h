#pragma once

#ifdef USE_ESP32

#include "equalizer.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace equalizer_speaker {

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

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }

  void add_peak_eq(double frequency, double q, double gain) {
    equalizer::filter_biquad new_filter;
    new_filter.set_peak_eq(frequency, q, gain);
    this->filters_.push_back(new_filter);
  }

 protected:
  esp_err_t start_();
  void stop_();

  static void equalizer_task(void *params);

  std::vector<equalizer::filter_biquad> filters_;

  TaskHandle_t task_handle_{nullptr};
  EventGroupHandle_t event_group_{nullptr};

  std::weak_ptr<RingBuffer> ring_buffer_;

  speaker::Speaker *output_speaker_{nullptr};

  bool task_created_{false};
};

}  // namespace equalizer_speaker
}  // namespace esphome

#endif
