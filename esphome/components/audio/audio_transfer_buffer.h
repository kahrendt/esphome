
#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esphome/components/speaker/speaker.h"

#include "esp_err.h"

#include <freertos/FreeRTOS.h>

namespace esphome {
namespace audio {

class AudioTransferBuffer {
 public:
  // AudioTransferBuffer(size_t buffer_size) : buffer_size_(buffer_size) { this->allocate_buffer_(); }
  // AudioTransferBuffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size)
  //     : ring_buffer_(ring_buffer), buffer_size_(buffer_size) {
  //   this->allocate_buffer_();
  // }
  AudioTransferBuffer() {}
  AudioTransferBuffer(size_t buffer_size) : buffer_size_(buffer_size) {}

  ~AudioTransferBuffer();

  virtual bool allocated_successfully();

  bool add_ring_buffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size) {
    this->buffer_size_ = buffer_size;
    this->allocate_buffer_();
    this->ring_buffer_ = ring_buffer;
    return this->allocated_successfully();
  }

  uint8_t *get_buffer_start() { return this->data_start_; }
  uint8_t *get_buffer_end() { return this->data_start_ + this->buffer_length_; }

  void decrease_buffer_length(size_t bytes);
  void increase_buffer_length(size_t bytes) { this->buffer_length_ += bytes; }

  size_t free() { return this->buffer_size_ - (this->buffer_length_ - (this->data_start_ - this->buffer_)); }
  size_t available() { return this->buffer_length_; }

  size_t capacity() { return this->buffer_size_; }

  virtual bool has_buffered_data() = 0;

 protected:
  void allocate_buffer_() {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    if (this->buffer_ != nullptr) {
      allocator.deallocate(this->buffer_, this->buffer_size_);
    }

    this->buffer_ = allocator.allocate(this->buffer_size_);
    this->data_start_ = this->buffer_;
    this->buffer_length_ = 0;
  }

  std::shared_ptr<RingBuffer> ring_buffer_;

  uint8_t *buffer_{nullptr};
  uint8_t *data_start_{nullptr};

  size_t buffer_size_{0};
  size_t buffer_length_;
};

class AudioOutTransferBuffer : public AudioTransferBuffer {
 public:
  // AudioOutTransferBuffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size)
  //     : AudioTransferBuffer(ring_buffer, buffer_size) {}
  // AudioOutTransferBuffer(speaker::Speaker *speaker, size_t buffer_size)
  //     : AudioTransferBuffer(buffer_size), speaker_(speaker) {}
  AudioOutTransferBuffer() {}
  AudioOutTransferBuffer(size_t buffer_size) : AudioTransferBuffer(buffer_size) { this->allocate_buffer_(); }

  size_t transfer_audio_out(TickType_t ticks_to_wait);

  bool add_speaker(speaker::Speaker *speaker, size_t buffer_size) {
    this->buffer_size_ = buffer_size;
    this->allocate_buffer_();
    this->speaker_ = speaker;
    return allocated_successfully();
  }

  bool has_buffered_data() override {
    if (this->speaker_ != nullptr) {
      return (this->speaker_->has_buffered_data() || (this->available() > 0));
    } else if (this->ring_buffer_.use_count() > 0) {
      return ((this->ring_buffer_->available() > 0) || (this->available() > 0));
    }
    return (this->available() > 0);
  }

  bool allocated_successfully() override;

 protected:
  speaker::Speaker *speaker_{nullptr};
};

class AudioInTransferBuffer : public AudioTransferBuffer {
 public:
  // AudioInTransferBuffer(std::shared_ptr<RingBuffer> &ring_buffer, size_t buffer_size)
  //     : AudioTransferBuffer(ring_buffer, buffer_size) {}
  size_t read_ring_buffer(TickType_t ticks_to_wait);

  bool has_buffered_data() override {
    if (this->ring_buffer_.use_count() > 0) {
      return ((this->ring_buffer_->available() > 0) || (this->available() > 0));
    }
    return (this->available() > 0);
  }
};

}  // namespace audio
}  // namespace esphome
