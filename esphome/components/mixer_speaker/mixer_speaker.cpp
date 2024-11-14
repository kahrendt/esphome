#ifdef USE_ESP32

#include "mixer_speaker.h"

namespace esphome {
namespace mixer_speaker {

static const UBaseType_t MIXER_TASK_PRIORITY = 10;

static const uint32_t MIXER_INPUT_RING_BUFFER_DURATION_MS = 50;
static const size_t TRANSFER_BUFFER_SIZE = 4096;

void InputSpeaker::loop() {
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

size_t InputSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
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

void InputSpeaker::start() {
  this->parent_->start(this->audio_stream_info_);
  this->state_ = speaker::STATE_RUNNING;
  this->stop_gracefully_ = false;
}

void InputSpeaker::stop() {
  if (this->ring_buffer_.use_count() > 0) {
    this->ring_buffer_->reset();
  }

  this->state_ = speaker::STATE_STOPPED;
}

void InputSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->parent_->get_output_speaker()->set_mute_state(mute_state);
}

void InputSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->parent_->get_output_speaker()->set_volume(volume);
}

bool InputSpeaker::has_buffered_data() const {
  if (this->ring_buffer_.use_count() > 0) {
    return (this->ring_buffer_->available() > 0);
  }
  return false;
}

void MixerSpeaker::start(audio::AudioStreamInfo &stream_info) {
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

  const size_t ring_buffer_size = MIXER_INPUT_RING_BUFFER_DURATION_MS * stream_info.get_bytes_per_ms();
  this->mixer_ = audio::AudioMixer::create(ring_buffer_size, TRANSFER_BUFFER_SIZE);

  if (this->mixer_ == nullptr) {
    this->status_set_error("Failed to allocate mixer buffers");
    return;
  }

  this->mixer_->start(this->output_speaker_, "mixer", MIXER_TASK_PRIORITY);

  this->set_retry(50, 2, [this](const uint8_t remaining_setup_attempts) {
    if ((this->mixer_->get_announcement_ring_buffer().use_count() == 0) ||
        (this->mixer_->get_media_ring_buffer().use_count() == 0)) {
      if (remaining_setup_attempts == 0) {
        this->status_set_error("Error starting the audio pipeline since the mixer hasn't finished allocating buffers");
      }
      return RetryResult::RETRY;
    }

    this->announcement_speaker_->set_sink(this->mixer_->get_announcement_ring_buffer());
    this->media_speaker_->set_sink(this->mixer_->get_media_ring_buffer());

    return RetryResult::DONE;
  });
}

}  // namespace mixer_speaker
}  // namespace esphome

#endif
