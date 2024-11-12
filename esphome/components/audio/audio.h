#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace audio {

struct AudioStreamInfo {
  bool operator==(const AudioStreamInfo &rhs) const {
    return (channels == rhs.channels) && (bits_per_sample == rhs.bits_per_sample) && (sample_rate == rhs.sample_rate);
  }
  bool operator!=(const AudioStreamInfo &rhs) const { return !operator==(rhs); }
  size_t get_bytes_per_sample() const { return bits_per_sample / 8; }
  uint32_t get_frames_per_ms() const { return sample_rate / 1000; }
  uint32_t get_samples_per_ms() const { return get_frames_per_ms() * channels; }
  size_t get_bytes_per_ms() const { return get_samples_per_ms() * get_bytes_per_sample(); }
  uint8_t channels = 1;
  uint8_t bits_per_sample = 16;
  uint32_t sample_rate = 16000;
};

}  // namespace audio
}  // namespace esphome
