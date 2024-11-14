#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_mixer.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

namespace esphome {
namespace mixer_speaker {

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
  void setup() override {}

  void start(audio::AudioStreamInfo &stream_info);

  void set_announcement_speaker(InputSpeaker *announcement_speaker) {
    this->announcement_speaker_ = announcement_speaker;
  }
  void set_media_speaker(InputSpeaker *media_speaker) { this->media_speaker_ = media_speaker; }

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }
  speaker::Speaker *get_output_speaker() { return this->output_speaker_; }

 protected:
  InputSpeaker *announcement_speaker_{nullptr};
  InputSpeaker *media_speaker_{nullptr};
  speaker::Speaker *output_speaker_{nullptr};

  optional<audio::AudioStreamInfo> audio_stream_info_;

  std::unique_ptr<audio::AudioMixer> mixer_;
};

}  // namespace mixer_speaker
}  // namespace esphome

#endif
