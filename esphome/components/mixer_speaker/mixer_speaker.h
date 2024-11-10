#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_mixer.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

namespace esphome {
namespace mixer_speaker {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

class MixerSpeaker;

class InputSpeaker : public speaker::Speaker, public Component {
 public:
  void setup() override {}

  void loop() override {
    if (this->state_ == speaker::STATE_RUNNING) {
      if (((millis() - this->last_seen_data_ms_) > this->timeout_ms_) && !this->has_buffered_data()) {
        this->state_ = speaker::STATE_STOPPED;
        this->stop_gracefully_ = false;
      }
      if (this->stop_gracefully_ && !this->has_buffered_data()) {
        this->state_ = speaker::STATE_STOPPED;
        this->stop_gracefully_ = false;
      }
    }
  }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override {
    this->last_seen_data_ms_ = millis();
    if (this->is_stopped()) {
      this->start();
    }
    size_t bytes_written = 0;
    if (this->ring_buffer_.use_count() > 0) {
      bytes_written = this->ring_buffer_->write_without_replacement(data, length, ticks_to_wait);
    }
    return bytes_written;
  }

  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override {
    if (this->ring_buffer_.use_count() > 0) {
      this->ring_buffer_->reset();
    }

    this->state_ = speaker::STATE_STOPPED;
  }
  void finish() override { this->stop_gracefully_ = true; }

  bool has_buffered_data() const override {
    if (this->ring_buffer_.use_count() > 0) {
      return (this->ring_buffer_->available() > 0);
    }
    return false;
  }

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

  void start(audio::AudioStreamInfo &stream_info) {
    if (!this->audio_stream_info_.has_value()) {
      this->audio_stream_info_ = stream_info;
      this->output_speaker_->set_audio_stream_info(stream_info);
    } else {
      if (stream_info != this->audio_stream_info_.value()) {
        printf("mismatching audio stream info, can't play");
      }
    }

    if (this->mixer_ != nullptr) {
      return;
    }

    this->mixer_ = audio::AudioMixer::create(8192 * 16, 4096);
    if (this->mixer_ == nullptr) {
      this->status_set_error("Failed to allocate mixer buffers");
      return;
    }

    this->mixer_->start(this->output_speaker_, "mixer", MIXER_TASK_PRIORITY);

    this->set_retry(50, 2, [this](const uint8_t remaining_setup_attempts) {
      if ((this->mixer_->get_announcement_ring_buffer().use_count() == 0) ||
          (this->mixer_->get_media_ring_buffer().use_count() == 0)) {
        if (remaining_setup_attempts == 0) {
          this->status_set_error(
              "Error starting the audio pipeline since the mixer hasn't finished allocating buffers");
        }
        return RetryResult::RETRY;
      }

      this->announcement_speaker_->set_sink(this->mixer_->get_announcement_ring_buffer());
      this->media_speaker_->set_sink(this->mixer_->get_media_ring_buffer());

      return RetryResult::DONE;
    });
  }

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
