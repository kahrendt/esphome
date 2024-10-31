
#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

#include <freertos/FreeRTOS.h>

namespace esphome {
namespace audio {

class AudioBuffer {
 public:
  AudioBuffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size)
      : ring_buffer_(ring_buffer), buffer_size_(buffer_size) {
    this->allocate_buffer_();
  }
  ~AudioBuffer();

  size_t read_ring_buffer(TickType_t ticks_to_wait);

  size_t write_ring_buffer(TickType_t ticks_to_wait);

  bool allocated_successfully();

  uint8_t *get_buffer_start() { return this->data_start_; }
  uint8_t *get_buffer_end() { return this->data_start_ + this->buffer_length_; }

  void increase_buffer_length(size_t bytes) { this->buffer_length_ += bytes; }
  void decrease_buffer_length(size_t bytes);

  size_t available() { return this->buffer_length_; }
  size_t free() { return this->buffer_size_ - (this->buffer_length_ - (this->data_start_ - this->buffer_)); }

  size_t capacity() { return this->buffer_size_; }

  std::shared_ptr<RingBuffer> &get_ring_buffer() { return this->ring_buffer_; }

 protected:
  void allocate_buffer_() {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->buffer_ = allocator.allocate(this->buffer_size_);
    this->data_start_ = this->buffer_;
    this->buffer_length_ = 0;
  }

  std::shared_ptr<RingBuffer> ring_buffer_;

  uint8_t *buffer_{nullptr};
  uint8_t *data_start_{nullptr};

  size_t buffer_size_;
  size_t buffer_length_;
};

}  // namespace audio
}  // namespace esphome
