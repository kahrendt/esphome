#include "audio_files.h"

namespace esphome {
namespace audio {

const char *media_player_file_type_to_string(MediaFileType file_type) {
  switch (file_type) {
    case MediaFileType::FLAC:
      return "FLAC";
    case MediaFileType::MP3:
      return "MP3";
    case MediaFileType::WAV:
      return "WAV";
    default:
      return "unknown";
  }
}

}  // namespace audio
}  // namespace esphome
