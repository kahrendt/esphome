#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

namespace esphome {
namespace mixer_speaker {

enum class EventType : uint8_t {
  STARTING = 0,
  STARTED,
  RUNNING,
  IDLE,
  STOPPING,
  STOPPED,
  WARNING = 255,
};

// Used for reporting the state of the mixer task
struct TaskEvent {
  EventType type;
  esp_err_t err;
};

enum class CommandEventType : uint8_t {
  STOP,  // Stop mixing to prepare for stopping the mixing task
  DUCK,  // Duck the media audio
};

// Used to send commands to the mixer task
struct CommandEvent {
  CommandEventType command;
  uint8_t decibel_reduction;
  size_t transition_samples = 0;
};

// Gives the Q15 fixed point scaling factor to reduce by 0 dB, 1dB, ..., 50 dB
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> DECIBEL_REDUCTION_TABLE = {
    32767, 29201, 26022, 23189, 20665, 18415, 16410, 14624, 13032, 11613, 10349, 9222, 8218, 7324, 6527, 5816, 5183,
    4619,  4116,  3668,  3269,  2913,  2596,  2313,  2061,  1837,  1637,  1459,  1300, 1158, 1032, 920,  820,  731,
    651,   580,   517,   461,   411,   366,   326,   291,   259,   231,   206,   183,  163,  146,  130,  116,  103};

class MixerSpeaker;

class InputSpeaker : public speaker::Speaker, public Component {
 public:
  void setup() override {}

  void loop() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;

  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override { this->stop_gracefully_ = true; }

  bool has_buffered_data() const override;

  void set_mute_state(bool mute_state) override;

  void set_volume(float volume) override;

  void set_sink(std::weak_ptr<RingBuffer> ring_buffer_) { this->ring_buffer_ = ring_buffer_.lock(); }

  audio::AudioStreamInfo get_audio_stream_info() { return this->audio_stream_info_; }

  void set_parent(MixerSpeaker *parent) { this->parent_ = parent; }

 protected:
  std::shared_ptr<RingBuffer> ring_buffer_;
  MixerSpeaker *parent_;

  uint32_t last_seen_data_ms_;
  uint32_t timeout_ms_{1000};
  bool stop_gracefully_{false};
};

class MixerSpeaker : public Component {
 public:
  void setup() override;

  void start(audio::AudioStreamInfo &stream_info);

  void set_announcement_speaker(InputSpeaker *announcement_speaker) {
    this->announcement_speaker_ = announcement_speaker;
  }
  void set_media_speaker(InputSpeaker *media_speaker) { this->media_speaker_ = media_speaker; }

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }
  speaker::Speaker *get_output_speaker() { return this->output_speaker_; }

  /// @brief Suspends the mixer task
  void suspend_task();
  /// @brief Resumes the mixer task
  void resume_task();

  /// @brief Sets the ducking level for the media stream in the mixer
  /// @param decibel_reduction (uint8_t) The dB reduction level. For example, 0 is no change, 10 is a reduction by 10 dB
  /// @param transition_samples (size_t) The number of samples to transition to the new ducking level over
  void set_ducking_reduction(uint8_t decibel_reduction, size_t transition_samples);

  void get_state() {
    TaskEvent event;
    while (this->read_event_(&event)) {
      if (event.type == EventType::WARNING) {
        // ESP_LOGD(TAG, "Mixer encountered an error: %s", esp_err_to_name(event.err));
        // this->status_set_error();
      }
    }
  }

 protected:
  /// @brief Reads a TaskEvent from the event queue indicating its current status
  /// @param event Pointer to TaskEvent object to store the event in
  /// @param ticks_to_wait The number of FreeRTOS ticks to wait for an event to appear on the queue. Defaults to 0.
  /// @return pdTRUE if successful, pdFALSE otherwise
  BaseType_t read_event_(TaskEvent *event, TickType_t ticks_to_wait = 0);

  /// @brief Sends a CommandEvent to the command queue
  /// @param command Pointer to CommandEvent object to be sent
  /// @param ticks_to_wait The number of FreeRTOS ticks to wait for an event to appear on the queue. Defaults to 0.
  /// @return pdTRUE if successful, pdFALSE otherwises
  BaseType_t send_command_(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY);

  /// @brief Mixes the media and announcement samples. If the resulting audio clips, the media samples are first
  /// scaled.
  /// @param media_buffer buffer for media samples
  /// @param announcement_buffer buffer for announcement samples
  /// @param combination_buffer buffer for the mixed samples
  /// @param samples_to_mix number of samples in the media and annoucnement buffers to mix together
  void mix_audio_samples_without_clipping_(int16_t *media_buffer, int16_t *announcement_buffer,
                                           int16_t *combination_buffer, size_t samples_to_mix);

  /// @brief Scales audio samples. Scales in place when audio_samples == output_buffer.
  /// @param audio_samples PCM int16 audio samples
  /// @param output_buffer Buffer to store the scaled samples
  /// @param scale_factor Q15 fixed point scaling factor
  /// @param samples_to_scale Number of samples to scale
  void scale_audio_samples_(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                            size_t samples_to_scale);

  static void audio_mixer_task(void *params);
  TaskHandle_t task_handle_{nullptr};

  InputSpeaker *announcement_speaker_{nullptr};
  InputSpeaker *media_speaker_{nullptr};
  speaker::Speaker *output_speaker_{nullptr};

  optional<audio::AudioStreamInfo> audio_stream_info_;

  // Reports events from the mixer task
  QueueHandle_t event_queue_{nullptr};

  // Stores commands to send the mixer task
  QueueHandle_t command_queue_{nullptr};

  std::weak_ptr<RingBuffer> media_ring_buffer_;
  std::weak_ptr<RingBuffer> announcement_ring_buffer_;

  size_t ring_buffer_size_;
  size_t transfer_buffer_size_;
};

}  // namespace mixer_speaker
}  // namespace esphome

#endif
