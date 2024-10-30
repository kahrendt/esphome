#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

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
  AudioBuffer(size_t ring_buffer_size, size_t buffer_size) : buffer_size_(buffer_size) {
    this->ring_buffer_ = std::move(RingBuffer::create(ring_buffer_size));
    this->allocate_buffer_();
  }
  ~AudioBuffer() {
    if (this->buffer_ != nullptr) {
      ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      allocator.deallocate(this->buffer_, this->buffer_size_);
    }
    // Release ownership of ring buffer
    this->ring_buffer_.reset();
  }

  size_t read_ring_buffer_(TickType_t ticks_to_wait) {
    if (!this->allocated_successfully()) {
      return 0;
    }

    // Shift data in buffer to start
    if (this->buffer_length_ > 0) {
      memmove(this->buffer_, this->buffer_current_, this->buffer_length_);
    }

    this->buffer_current_ = this->buffer_;

    size_t bytes_read = 0;

    size_t bytes_to_read = this->buffer_size_ - this->buffer_length_;

    if (bytes_to_read > 0) {
      uint8_t *new_data_start = this->buffer_ + this->buffer_length_;
      bytes_read = this->ring_buffer_->read((void *) new_data_start, bytes_to_read, ticks_to_wait);

      this->buffer_length_ += bytes_read;
    }
    return bytes_read;
  }

  size_t write_ring_buffer_(TickType_t ticks_to_wait) {
    if (!this->allocated_successfully()) {
      return 0;
    }

    size_t bytes_written = 0;
    if (this->buffer_length_ > 0) {
      bytes_written =
          this->ring_buffer_->write_without_replacement((void *) this->buffer_, this->buffer_length_, ticks_to_wait);
      this->buffer_length_ -= bytes_written;

      // Shift unwritten data to the start of the buffer
      memmove(this->buffer_, this->buffer_ + bytes_written, this->buffer_length_);
      this->buffer_current_ = this->buffer_ + this->buffer_length_;
    }
    return bytes_written;
  }

  bool allocated_successfully() {
    if (this->ring_buffer_.use_count() && (this->buffer_ != nullptr)) {
      return true;
    }
    return false;
  }

  uint8_t *get_current_buffer() { return this->buffer_current_; }
  void offset_buffer_length(size_t bytes) { this->buffer_length_ += bytes; }

  size_t available() { return this->buffer_length_; }
  size_t free() { return this->buffer_size_ - this->buffer_length_; }

  std::shared_ptr<RingBuffer> &get_ring_buffer() { return this->ring_buffer_; }

 protected:
  void allocate_buffer_() {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->buffer_ = allocator.allocate(this->buffer_size_);
    this->buffer_current_ = this->buffer_;
    this->buffer_length_ = 0;
  }
  uint8_t *buffer_{nullptr};
  uint8_t *buffer_current_{nullptr};

  std::shared_ptr<RingBuffer> ring_buffer_;

  size_t buffer_size_;
  size_t buffer_length_;
};

class AudioOutputBufferStage {
 public:
  AudioOutputBufferStage(std::shared_ptr<RingBuffer> &output_ring_buffer, size_t buffer_size) {
    this->output_audio_buffer_ = new AudioBuffer(output_ring_buffer, buffer_size);
  }
  ~AudioOutputBufferStage() { delete this->output_audio_buffer_; }

 protected:
  AudioBuffer *output_audio_buffer_;
};

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
