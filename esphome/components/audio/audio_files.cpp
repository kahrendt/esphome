#include "audio_files.h"

namespace esphome {
namespace audio {

const char *media_player_file_type_to_string(AudioFileType file_type) {
  switch (file_type) {
    case AudioFileType::FLAC:
      return "FLAC";
    case AudioFileType::MP3:
      return "MP3";
    case AudioFileType::WAV:
      return "WAV";
    default:
      return "unknown";
  }
}

}  // namespace audio
}  // namespace esphome
