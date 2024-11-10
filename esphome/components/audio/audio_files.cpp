#include "audio_files.h"

namespace esphome {
namespace audio {

const char *audio_file_type_to_string(AudioFileType file_type) {
  switch (file_type) {
#ifdef USE_AUDIO_FLAC_SUPPORT
    case AudioFileType::FLAC:
      return "FLAC";
#endif
#ifdef USE_AUDIO_MP3_SUPPORT
    case AudioFileType::MP3:
      return "MP3";
#endif
    case AudioFileType::WAV:
      return "WAV";
    default:
      return "unknown";
  }
}

}  // namespace audio
}  // namespace esphome
