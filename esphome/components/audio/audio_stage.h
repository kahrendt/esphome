#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

namespace esphome {
namespace audio {

// class AudioStage {
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

// class AudioInputStage : public AudioStage {
class AudioInputStage {
 public:
  AudioInputStage(std::shared_ptr<RingBuffer> &input_ring_buffer, size_t buffer_size)
      : input_buffer_size_(buffer_size) {
    this->input_ring_buffer_ = input_ring_buffer;
  }
  ~AudioInputStage() {
    if (this->input_buffer_ != nullptr) {
      ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      allocator.deallocate(this->input_buffer_, this->input_buffer_size_);
      this->input_buffer_ = nullptr;
    }
  }

 protected:
  esp_err_t allocate_input_buffer_() {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    if (this->input_buffer_ == nullptr)
      this->input_buffer_ = allocator.allocate(this->input_buffer_size_);
    if (this->input_buffer_ == nullptr)
      return ESP_ERR_NO_MEM;

    this->input_buffer_current_ = this->input_buffer_;
    this->input_buffer_length_ = 0;
    return ESP_OK;
  }

  size_t read_ring_buffer_(TickType_t ticks_to_wait) {
    if (this->input_buffer_length_ > 0) {
      memmove(this->input_buffer_, this->input_buffer_current_, this->input_buffer_length_);
    }
    this->input_buffer_current_ = this->input_buffer_;

    size_t bytes_read = 0;

    // read in new ring buffer data to fill the remaining input buffer
    size_t bytes_to_read = this->input_buffer_size_ - this->input_buffer_length_;

    if (bytes_to_read > 0) {
      uint8_t *new_audio_data = this->input_buffer_ + this->input_buffer_length_;
      bytes_read = this->input_ring_buffer_->read((void *) new_audio_data, bytes_to_read, ticks_to_wait);

      this->input_buffer_length_ += bytes_read;
    }

    return bytes_read = 0;
  }

  std::shared_ptr<RingBuffer> input_ring_buffer_;

  uint8_t *input_buffer_{nullptr};
  uint8_t *input_buffer_current_{nullptr};

  size_t input_buffer_length_;  // Amount of data currently stored in output buffer (in bytes)
  size_t input_buffer_size_;    // Total bytes to allocate in output buffer
};

// class AudioOutputStage : public AudioStage {
class AudioOutputStage {
 public:
  AudioOutputStage(std::shared_ptr<RingBuffer> output_ring_buffer, size_t buffer_size)
      : output_ring_buffer_(output_ring_buffer), output_buffer_size_(buffer_size) {}

  ~AudioOutputStage() {
    if (this->output_buffer_ != nullptr) {
      ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      allocator.deallocate(this->output_buffer_, this->output_buffer_size_);
      this->output_buffer_ = nullptr;
    }
  }

 protected:
  esp_err_t allocate_output_buffer_() {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    if (this->output_buffer_ == nullptr)
      this->output_buffer_ = allocator.allocate(this->output_buffer_size_);
    if (this->output_buffer_ == nullptr)
      return ESP_ERR_NO_MEM;

    this->output_buffer_length_ = 0;
    return ESP_OK;
  }

  size_t write_ring_buffer_(TickType_t ticks_to_wait) {
    size_t bytes_written = 0;
    if (this->output_buffer_length_ > 0) {
      bytes_written = this->output_ring_buffer_->write_without_replacement((void *) this->output_buffer_,
                                                                           this->output_buffer_length_, ticks_to_wait);
      this->output_buffer_length_ -= bytes_written;

      // Shift remaining data to the start of the transfer buffer
      memmove(this->output_buffer_, this->output_buffer_ + bytes_written, this->output_buffer_length_);
    }
    return bytes_written;  // No data to write
  }

  std::shared_ptr<RingBuffer> output_ring_buffer_;

  uint8_t *output_buffer_{nullptr};

  size_t output_buffer_length_;  // Amount of data currently stored in output buffer (in bytes)
  size_t output_buffer_size_;    // Total bytes to allocate in output buffer
};
}  // namespace audio
}  // namespace esphome
