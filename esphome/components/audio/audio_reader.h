#pragma once

#ifdef USE_ESP_IDF

#include "audio_files.h"
#include "audio_stage.h"

#include "esphome/core/ring_buffer.h"

#include <esp_http_client.h>

namespace esphome {
namespace audio {

enum class AudioReaderState : uint8_t {
  READING = 0,
  FINISHED,
  FAILED,
};

class AudioReader : public AudioOutputStage {
 public:
  AudioReader(std::shared_ptr<esphome::RingBuffer> &output_ring_buffer, size_t output_buffer_size)
      : AudioOutputStage(output_ring_buffer, output_buffer_size) {}
  ~AudioReader();

  esp_err_t start(const std::string &uri, AudioFileType &file_type);
  esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);

  AudioReaderState read();

 protected:
  AudioReaderState file_read_();
  AudioReaderState http_read_();

  void cleanup_connection_();

  ssize_t no_data_read_count_;

  const uint8_t *output_buffer_current_{nullptr};

  esp_http_client_handle_t client_{nullptr};

  AudioFile *current_audio_file_{nullptr};
};
}  // namespace audio
}  // namespace esphome

#endif
