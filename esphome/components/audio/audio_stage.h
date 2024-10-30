#pragma once

#include <cstdint>
#include <cstddef>

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

namespace esphome {
namespace audio {

class AudioStage {
 protected:
  esp_err_t allocate_buffer_(uint8_t **buffer, size_t buffer_size) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    if (*buffer == nullptr)
      *buffer = allocator.allocate(buffer_size);

    if (*buffer == nullptr)
      return ESP_ERR_NO_MEM;

    return ESP_OK;
  }

  void deallocate_buffer_(uint8_t **buffer, size_t buffer_size) {
    if (*buffer != nullptr) {
      ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      allocator.deallocate(*buffer, buffer_size);
    }
  }
};

class AudioInputStage {
 public:
  AudioInputStage(std::shared_ptr<RingBuffer> input_ring_buffer) : input_ring_buffer_(input_ring_buffer) {}

 protected:
  std::shared_ptr<RingBuffer> input_ring_buffer_;
};

class AudioOutputStage : public AudioStage {
 public:
  AudioOutputStage(std::shared_ptr<RingBuffer> output_ring_buffer, size_t buffer_size)
      : output_ring_buffer_(output_ring_buffer), output_buffer_size_(buffer_size) {}

  ~AudioOutputStage() { this->deallocate_buffer_(&this->output_buffer_, this->output_buffer_size_); }

 protected:
  esp_err_t allocate_output_buffer_() {
    printf("allocating %d bytes for output buffer\n", this->output_buffer_size_);
    return this->allocate_buffer_(&this->output_buffer_, this->output_buffer_size_);
  }

  std::shared_ptr<RingBuffer> output_ring_buffer_;

  uint8_t *output_buffer_{nullptr};

  size_t output_buffer_length_;  // Amount of data currently stored in output buffer (in bytes)
  size_t output_buffer_size_;    // Total bytes to allocate in output buffer
};
}  // namespace audio
}  // namespace esphome
