#include "mixer_speaker.h"

namespace esphome {
namespace mixer_speaker {

void InputSpeaker::start() {
  this->parent_->start(this->audio_stream_info_);
  this->state_ = speaker::STATE_RUNNING;
  this->stop_gracefully_ = false;
}

void InputSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->parent_->get_output_speaker()->set_mute_state(mute_state);
}

void InputSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  this->parent_->get_output_speaker()->set_volume(volume);
}

}  // namespace mixer_speaker
}  // namespace esphome
