#pragma once

#ifdef USE_ESP_IDF

#include "audio_files.h"
#include "esphome/core/ring_buffer.h"

#include <esp_http_client.h>

namespace esphome {
namespace audio {

enum class AudioReaderState : uint8_t {
  READING = 0,
  FINISHED,
  FAILED,
};

class AudioReader {
 public:
  AudioReader(std::shared_ptr<esphome::RingBuffer> &output_ring_buffer, size_t transfer_buffer_size);
  ~AudioReader();

  esp_err_t start(const std::string &uri, AudioFileType &file_type);
  esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);

  AudioReaderState read();

 protected:
  esp_err_t allocate_buffers_();

  AudioReaderState file_read_();
  AudioReaderState http_read_();

  void cleanup_connection_();

  std::shared_ptr<esphome::RingBuffer> output_ring_buffer_;

  size_t transfer_buffer_length_;  // Amount of data currently stored in transfer buffer (in bytes)
  size_t transfer_buffer_size_;    // Capacity of transfer buffer (in bytes)

  ssize_t no_data_read_count_;

  uint8_t *transfer_buffer_{nullptr};
  const uint8_t *transfer_buffer_current_{nullptr};

  esp_http_client_handle_t client_{nullptr};

  AudioFile *current_audio_file_{nullptr};
};
}  // namespace audio
}  // namespace esphome

#endif
