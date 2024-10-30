#pragma once

#include <cstdint>
#include <cstddef>

namespace esphome {
namespace audio {

enum class AudioFileType : uint8_t {
  NONE = 0,
  WAV,
  MP3,
  FLAC,
};
const char *audio_file_type_to_string(AudioFileType file_type);

struct AudioFile {
  const uint8_t *data;
  size_t length;
  AudioFileType file_type;
};

}  // namespace audio
}  // namespace esphome
