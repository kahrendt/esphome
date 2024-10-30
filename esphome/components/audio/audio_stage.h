#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

namespace esphome {
namespace audio {

// class AudioBuffer {
//  protected:
//   esp_err_t allocate_buffer_(uint8_t **buffer, size_t buffer_size) {
//     ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
//     if (*buffer == nullptr)
//       *buffer = allocator.allocate(buffer_size);

//     if (*buffer == nullptr)
//       return ESP_ERR_NO_MEM;

//     return ESP_OK;
//   }

//   void deallocate_buffer_(uint8_t **buffer, size_t buffer_size) {
//     if (*buffer != nullptr) {
//       ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
//       allocator.deallocate(*buffer, buffer_size);
//       *buffer = nullptr;
//     }
//   }
// };

/******************************************************************* */
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
      *buffer = nullptr;
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
    return this->allocate_buffer_(&this->output_buffer_, this->output_buffer_size_);
  }

  bool write_ring_buffer_(TickType_t ticks_to_wait) {
    if (this->output_buffer_length_ > 0) {
      size_t bytes_written = this->output_ring_buffer_->write_without_replacement(
          (void *) this->output_buffer_, this->output_buffer_length_, ticks_to_wait);
      this->output_buffer_length_ -= bytes_written;

      // Shift remaining data to the start of the transfer buffer
      memmove(this->output_buffer_, this->output_buffer_ + bytes_written, this->output_buffer_length_);

      return true;  // Had some data to write
    }
    return false;  // No data to write
  }

  std::shared_ptr<RingBuffer> output_ring_buffer_;

  uint8_t *output_buffer_{nullptr};

  size_t output_buffer_length_;  // Amount of data currently stored in output buffer (in bytes)
  size_t output_buffer_size_;    // Total bytes to allocate in output buffer
};
}  // namespace audio
}  // namespace esphome
