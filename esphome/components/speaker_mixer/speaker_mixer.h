#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace speaker_mixer {

class SpeakerMixer;

class SourceSpeaker : public speaker::Speaker, public Component {
 public:
  void loop() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  bool has_buffered_data() const override;

  /// @brief Mute state changes are passed to the parent's output speaker
  void set_mute_state(bool mute_state) override;

  /// @brief Volume state changes are passed to the parent's output speaker
  void set_volume(float volume) override;

  /// @brief Transfers audio from the ring buffer into the transfer buffer. Ducks audio while transferring.
  /// @param ticks_to_wait FreeRTOS ticks to wait while waiting to read from the ring buffer.
  /// @return Number of bytes transferred from the ring buffer.
  size_t process_data_from_source(TickType_t ticks_to_wait);

  /// @brief Sets the ducking level for the source speaker.
  /// @param decibel_reduction (uint8_t) The dB reduction level. For example, 0 is no change, 10 is a reduction by 10 dB
  /// @param duration (uint32_t) The number of milliseconds to transition from the current level to the new level
  void apply_ducking(uint8_t decibel_reduction, uint32_t duration);

  audio::AudioStreamInfo get_audio_stream_info() const { return this->audio_stream_info_; }

  void set_parent(SpeakerMixer *parent) { this->parent_ = parent; }
  void set_timeout(uint32_t ms) { this->timeout_ms_ = ms; }

  std::weak_ptr<audio::AudioSourceTransferBuffer> get_transfer_buffer() { return this->transfer_buffer_; }

 protected:
  friend class SpeakerMixer;
  /// @brief Ducks audio samples by a specified amount. When changing the ducking amount, it can transition gradually
  /// over a specified amount of samples.
  /// @param input_buffer buffer with audio samples to be ducked in place
  /// @param input_samples_to_duck number of samples to process in ``input_buffer``
  /// @param current_ducking_db_reduction (passed by reference) the current dB reduction
  /// @param ducking_transition_samples_remaining (passed by reference) total number of samples left before the the
  ///         transition is finished
  /// @param samples_per_ducking_step total number of samples per ducking step for the transition
  /// @param db_change_per_ducking_step the change in dB reduction per step
  static void duck_samples(int16_t *input_buffer, uint32_t input_samples_to_duck, int8_t &current_ducking_db_reduction,
                           size_t &ducking_transition_samples_remaining, size_t samples_per_ducking_step,
                           int8_t db_change_per_ducking_step);

  SpeakerMixer *parent_;

  std::shared_ptr<audio::AudioSourceTransferBuffer> transfer_buffer_;
  std::weak_ptr<RingBuffer> ring_buffer_;

  uint32_t last_seen_data_ms_{0};
  optional<uint32_t> timeout_ms_;
  bool stop_gracefully_{false};

  int8_t target_ducking_db_reduction_{0};
  int8_t current_ducking_db_reduction_{0};
  int8_t db_change_per_ducking_step_{1};
  size_t ducking_transition_samples_remaining_{0};
  size_t samples_per_ducking_step_{0};
};

class SpeakerMixer : public Component {
 public:
  void setup() override;
  void loop() override;

  void set_primary_speaker(SourceSpeaker *primary_speaker) { this->primary_speaker_ = primary_speaker; }
  void set_secondary_speaker(SourceSpeaker *secondary_speaker) { this->secondary_speaker_ = secondary_speaker; }

  /// @brief Starts the mixer task. Called by a source speaker giving the current audio stream information
  /// @param stream_info The calling source speakers audio stream information
  /// @return ESP_ERR_NOT_SUPPORTED if the incoming stream is incomptabile due to unsupported bits per sample
  ///         ESP_ERR_INVALID_ARG if the incoming stream is incompatible to be mixed with the other input audio stream
  ///         ESP_ERR_INVALID_STATE if the mixer task fails to start
  ///         ESP_OK if the incoming stream is compatible and the mixer task starts
  esp_err_t start(audio::AudioStreamInfo &stream_info);

  void stop();

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }
  speaker::Speaker *get_output_speaker() const { return this->output_speaker_; }

 protected:
  // friend class SourceSpeaker;

  /// @brief Copies audio frames from the input buffer to the output buffer taking into account the number of channels
  /// in each stream. If the output stream has more channels, the input samples are duplicated. If the output stream has
  /// less channels, the extra channel input samples are dropped.
  /// @param input_buffer
  /// @param input_stream_info
  /// @param output_buffer
  /// @param output_stream_info
  /// @param frames_to_transfer number of frames (consisting of a sample for each channel) to copy from the input buffer
  /// @param bytes_read passed by reference indicating the number of bytes read from the input buffer
  /// @param bytes_written passed by reference indicating the number of bytes written to the output buffer
  static void copy_frames(int16_t *input_buffer, audio::AudioStreamInfo input_stream_info, int16_t *output_buffer,
                          audio::AudioStreamInfo output_stream_info, uint32_t frames_to_transfer, size_t &bytes_read,
                          size_t &bytes_written);

  /// @brief Mixes the primary and secondary streams. If the resulting audio clips, the secondary samples are first
  /// scaled.
  /// @param primary_buffer samples buffer for the primary stream
  /// @param primary_stream_info stream info for the primary stream
  /// @param secondary_buffer samples buffer for secondary stream
  /// @param secondary_stream_info stream info for the secondary stream
  /// @param output_buffer buffer for the mixed samples
  /// @param output_stream_info stream info for the output buffer
  /// @param frames_to_mix number of frames in the primary and secondary buffers to mix together
  static void mix_audio_samples_without_clipping(int16_t *primary_buffer, audio::AudioStreamInfo primary_stream_info,
                                                 int16_t *secondary_buffer,
                                                 audio::AudioStreamInfo secondary_stream_info, int16_t *output_buffer,
                                                 audio::AudioStreamInfo output_stream_info, size_t frames_to_mix);

  static void audio_mixer_task(void *params);

  TaskHandle_t task_handle_{nullptr};
  EventGroupHandle_t event_group_{nullptr};

  SourceSpeaker *primary_speaker_{nullptr};
  SourceSpeaker *secondary_speaker_{nullptr};
  speaker::Speaker *output_speaker_{nullptr};

  bool task_created_{false};
  optional<audio::AudioStreamInfo> audio_stream_info_;
};

}  // namespace speaker_mixer
}  // namespace esphome

#endif
