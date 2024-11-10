#pragma once

#include "esphome/core/defines.h"

#include <cstdint>
#include <cstddef>

namespace esphome {
namespace audio {

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
const char *audio_file_type_to_string(AudioFileType file_type);

struct AudioFile {
  const uint8_t *data;
  size_t length;
  AudioFileType file_type;
};

}  // namespace audio
}  // namespace esphome
