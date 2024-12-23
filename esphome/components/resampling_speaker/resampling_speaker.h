#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace resampling_speaker {

class ResamplingSpeaker : public Component, public speaker::Speaker {
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

 protected:
  esp_err_t start_();
  void stop_();

  inline bool requires_resampling_() const {
    return (this->audio_stream_info_.sample_rate != this->target_sample_rate_);
  }

  static void resample_task(void *params);

  TaskHandle_t task_handle_{nullptr};
  EventGroupHandle_t event_group_{nullptr};

  std::weak_ptr<RingBuffer> ring_buffer_;

  speaker::Speaker *output_speaker_{nullptr};

  bool task_created_{false};

  uint32_t target_sample_rate_;
};

}  // namespace resampling_speaker
}  // namespace esphome

#endif
