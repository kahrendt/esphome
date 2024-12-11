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

// Gives the Q15 fixed point scaling factor to reduce by 0 dB, 1dB, ..., 50 dB
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> DECIBEL_REDUCTION_TABLE = {
    32767, 29201, 26022, 23189, 20665, 18415, 16410, 14624, 13032, 11613, 10349, 9222, 8218, 7324, 6527, 5816, 5183,
    4619,  4116,  3668,  3269,  2913,  2596,  2313,  2061,  1837,  1637,  1459,  1300, 1158, 1032, 920,  820,  731,
    651,   580,   517,   461,   411,   366,   326,   291,   259,   231,   206,   183,  163,  146,  130,  116,  103};

class SpeakerMixer;

class SourceSpeaker : public speaker::Speaker, public Component, public audio::AudioSourceTransferBuffer {
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

  audio::AudioStreamInfo get_audio_stream_info() { return this->audio_stream_info_; }

  void set_parent(SpeakerMixer *parent) { this->parent_ = parent; }

  /// @brief Transfers audio from the ring buffer into the transfer buffer. Ducks audio while transferring.
  /// @param ticks_to_wait FreeRTOS ticks to wait while waiting to read from the ring buffer.
  /// @return Number of bytes transferred from the ring buffer.
  size_t transfer_data_from_source(TickType_t ticks_to_wait) override;

  /// @brief Sets the ducking level for this source speaker.
  /// @param decibel_reduction (uint8_t) The dB reduction level. For example, 0 is no change, 10 is a reduction by 10 dB
  /// @param duration (uint32_t) The number of milliseconds to transition from the current level to the new level
  void set_ducking_reduction(uint8_t decibel_reduction, uint32_t duration);

 protected:
  static void duck_samples(int16_t *input_buffer, uint32_t input_samples_to_duck, int8_t &current_ducking_db_reduction,
                           size_t &ducking_transition_samples_remaining, size_t samples_per_ducking_step,
                           int8_t db_change_per_ducking_step);

  SpeakerMixer *parent_;

  uint32_t last_seen_data_ms_;
  uint32_t timeout_ms_{1000};
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

  /// @brief Starts the mixer task. Called by a source speaker giving the current audio stream information
  /// @param stream_info The calling source speakers audio stream information
  /// @return ESP_ERR_NOT_SUPPORTED if the incoming stream is incomptabile due to unsupported bits per sample
  ///         ESP_ERR_INVALID_ARG if the incoming stream is incompatible to be mixed with the other input audio stream
  ///         ESP_ERR_INVALID_STATE if the mixer task fails to start
  ///         ESP_OK if the incoming stream is compatible and the mixer task starts
  esp_err_t start(audio::AudioStreamInfo &stream_info);

  void set_primary_speaker(SourceSpeaker *primary_speaker) { this->primary_speaker_ = primary_speaker; }
  void set_secondary_speaker(SourceSpeaker *secondary_speaker) { this->secondary_speaker_ = secondary_speaker; }

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }
  speaker::Speaker *get_output_speaker() { return this->output_speaker_; }

 protected:
  /// @brief Mixes the primary and secondary streams. If the resulting audio clips, the secondary samples are first
  /// scaled.
  /// @param primary_buffer samples buffer for the primary stream
  /// @param primary_stream_info stream info for the primary stream
  /// @param secondary_buffer samples buffer for secondary stream
  /// @param secondary_stream_info stream info for the secondary stream
  /// @param output_buffer buffer for the mixed samples
  /// @param output_stream_info stream info for the output buffer
  /// @param frames_to_mix number of frames in the primary and secondary buffers to mix together
  void mix_audio_samples_without_clipping_(int16_t *primary_buffer, audio::AudioStreamInfo primary_stream_info,
                                           int16_t *secondary_buffer, audio::AudioStreamInfo secondary_stream_info,
                                           int16_t *output_buffer, audio::AudioStreamInfo output_stream_info,
                                           size_t frames_to_mix);

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
  void copy_frames_(int16_t *input_buffer, audio::AudioStreamInfo input_stream_info, int16_t *output_buffer,
                    audio::AudioStreamInfo output_stream_info, uint32_t frames_to_transfer, size_t &bytes_read,
                    size_t &bytes_written);

  static void audio_mixer_task(void *params);

  TaskHandle_t task_handle_{nullptr};
  EventGroupHandle_t event_group_{nullptr};

  SourceSpeaker *primary_speaker_{nullptr};
  SourceSpeaker *secondary_speaker_{nullptr};
  speaker::Speaker *output_speaker_{nullptr};

  bool task_created_{false};
  optional<audio::AudioStreamInfo> audio_stream_info_;

  size_t ring_buffer_size_;
  size_t transfer_buffer_size_;
};

}  // namespace speaker_mixer
}  // namespace esphome

#endif
