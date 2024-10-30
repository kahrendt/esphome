#pragma once

#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace audio {

class AudioInputStage {
 public:
  AudioInputStage(esphome::RingBuffer *input_ring_buffer) : input_ring_buffer_(input_ring_buffer) {}

 protected:
  esphome::RingBuffer *input_ring_buffer_;
};

class AudioOutputStage {
 public:
  AudioOutputStage(esphome::RingBuffer *output_ring_buffer) : output_ring_buffer_(output_ring_buffer) {}

 protected:
  esphome::RingBuffer *output_ring_buffer_;
};
}  // namespace audio
}  // namespace esphome
