#pragma once

#ifdef USE_ESP_IDF

#include "audio_files.h"
#include "audio_transfer_buffer.h"

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
  // AudioReader(std::shared_ptr<esphome::RingBuffer> &output_ring_buffer, size_t output_buffer_size) {
  //   this->output_transfer_buffer_ = make_unique<AudioSinkTransferBuffer>(output_ring_buffer, output_buffer_size);
  // }
  AudioReader(size_t buffer_size) : buffer_size_(buffer_size) {}
  ~AudioReader();

  bool add_ring_buffer(std::weak_ptr<esphome::RingBuffer> output_ring_buffer) {
    if (current_audio_file_ != nullptr) {
      // A tranfser buffer isn't ncessary for a local file
      this->file_ring_buffer_ = output_ring_buffer.lock();
      return true;
    }

    this->output_transfer_buffer_ = make_unique<AudioSinkTransferBuffer>(this->buffer_size_);
    this->output_transfer_buffer_->set_sink(output_ring_buffer);
    return this->output_transfer_buffer_->allocated_successfully();
  }

  esp_err_t start(const std::string &uri, AudioFileType &file_type);
  esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);

  AudioReaderState read();

 protected:
  AudioReaderState file_read_();
  AudioReaderState http_read_();

  std::shared_ptr<RingBuffer> file_ring_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;
  void cleanup_connection_();

  size_t buffer_size_;
  ssize_t no_data_read_count_;

  esp_http_client_handle_t client_{nullptr};

  AudioFile *current_audio_file_{nullptr};
  const uint8_t *file_current_{nullptr};
};
}  // namespace audio
}  // namespace esphome

#endif
