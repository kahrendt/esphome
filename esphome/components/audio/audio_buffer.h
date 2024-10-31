
#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

namespace esphome {
namespace audio {

class AudioBuffer {
 public:
  AudioBuffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size)
      : ring_buffer_(ring_buffer), buffer_size_(buffer_size) {
    this->allocate_buffer_();
  }
  ~AudioBuffer() {
    if (this->buffer_ != nullptr) {
      ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      allocator.deallocate(this->buffer_, this->buffer_size_);
    }
  }

  size_t read_ring_buffer(TickType_t ticks_to_wait) {
    if (!this->allocated_successfully()) {
      return 0;
    }

    // // Shift data in buffer to start
    // if (this->buffer_length_ > 0) {
    //   memmove(this->buffer_, this->data_start_, this->buffer_length_);
    // }

    // this->data_start_ = this->buffer_;

    size_t bytes_read = 0;

    // size_t bytes_to_read = this->buffer_size_ - this->buffer_length_;

    // if (bytes_to_read > 0) {
    //   uint8_t *new_data_start = this->buffer_ + this->buffer_length_;
    //   bytes_read = this->ring_buffer_->read((void *) new_data_start, bytes_to_read, ticks_to_wait);

    //   this->buffer_length_ += bytes_read;
    // }
    return bytes_read;
  }

  size_t write_ring_buffer(TickType_t ticks_to_wait) {
    if (!this->allocated_successfully()) {
      return 0;
    }

    size_t bytes_written = 0;
    if ((this->buffer_length_ > 0) && (this->available())) {
      bytes_written =
          this->ring_buffer_->write_without_replacement((void *) this->data_start_, this->available(), ticks_to_wait);
      this->decrease_buffer_length(bytes_written);

      // Shift unwritten data to the start of the buffer
      memmove(this->buffer_, this->data_start_, this->buffer_length_);
      this->data_start_ = this->buffer_;
    }
    return bytes_written;
  }

  bool allocated_successfully() {
    if (this->ring_buffer_.use_count() && (this->buffer_ != nullptr)) {
      return true;
    }
    return false;
  }

  uint8_t *get_buffer_start() { return this->data_start_; }
  uint8_t *get_buffer_end() { return this->data_start_ + this->buffer_length_; }

  void increase_buffer_length(size_t bytes) { this->buffer_length_ += bytes; }
  void decrease_buffer_length(size_t bytes) {
    this->buffer_length_ -= bytes;
    this->data_start_ += bytes;
  }

  size_t available() { return this->buffer_length_; }
  size_t free() { return this->buffer_size_ - (this->buffer_length_ - (this->data_start_ - this->buffer_)); }

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
