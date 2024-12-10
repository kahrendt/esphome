#pragma once

#include "esphome/core/defines.h"

#include <cstdint>

namespace esphome {
namespace audio {

struct AudioStreamInfo {
  bool operator==(const AudioStreamInfo &rhs) const {
    return (channels == rhs.channels) && (bits_per_sample == rhs.bits_per_sample) && (sample_rate == rhs.sample_rate);
  }
  bool operator!=(const AudioStreamInfo &rhs) const { return !operator==(rhs); }
  size_t get_bytes_per_sample() const { return bits_per_sample / 8; }
  uint32_t get_bytes_per_frame() const { return channels * bits_per_sample / 8; }
  uint32_t get_frames_per_ms() const { return sample_rate / 1000; }
  uint32_t get_samples_per_ms() const { return get_frames_per_ms() * channels; }
  size_t get_bytes_per_ms() const { return get_samples_per_ms() * get_bytes_per_sample(); }
  uint8_t channels = 1;
  uint8_t bits_per_sample = 16;
  uint32_t sample_rate = 16000;
};

/// @brief Scales audio samples. Scales in place when audio_samples == output_buffer.
/// @param audio_samples PCM int16 audio samples
/// @param output_buffer Buffer to store the scaled samples
/// @param scale_factor Q15 fixed point scaling factor
/// @param samples_to_scale Number of samples to scale
void scale_audio_samples(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor, size_t samples_to_scale);

enum class AudioFileType : uint8_t {
  NONE = 0,
#ifdef USE_AUDIO_FLAC_SUPPORT
  FLAC,
#endif
#ifdef USE_AUDIO_MP3_SUPPORT
  MP3,
#endif
  WAV,
};

struct AudioFile {
  const uint8_t *data;
  size_t length;
  AudioFileType file_type;
};

/// @brief Helper functions to convert file type to a const char string
/// @param file_type
/// @return const char pointer to the readable file type
const char *audio_file_type_to_string(AudioFileType file_type);

}  // namespace audio
}  // namespace esphome
