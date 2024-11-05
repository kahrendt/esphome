#include "audio_transfer_buffer.h"

namespace esphome {
namespace audio {
AudioTransferBuffer::~AudioTransferBuffer() {
  if (this->buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->buffer_, this->buffer_size_);
  }
}

void AudioTransferBuffer::clear_buffered_data() {
  this->buffer_length_ = 0;
  if (this->ring_buffer_.use_count() > 0) {
    this->ring_buffer_->reset();
  }
}

bool AudioTransferBuffer::has_buffered_data() {
  if (this->ring_buffer_.use_count() > 0) {
    return ((this->ring_buffer_->available() > 0) || (this->available() > 0));
  }
  return (this->available() > 0);
}

bool AudioTransferBuffer::add_ring_buffer_(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size) {
  this->buffer_size_ = buffer_size;
  this->allocate_buffer_();
  this->ring_buffer_ = ring_buffer.lock();
  return this->allocated_successfully();
}

void AudioTransferBuffer::allocate_buffer_() {
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  if (this->buffer_ != nullptr) {
    allocator.deallocate(this->buffer_, this->buffer_size_);
  }

  this->buffer_ = allocator.allocate(this->buffer_size_);
  this->data_start_ = this->buffer_;
  this->buffer_length_ = 0;
}

bool AudioSinkTransferBuffer::add_sink(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size) {
  return this->add_ring_buffer_(ring_buffer, buffer_size);
}

bool AudioSinkTransferBuffer::add_sink(speaker::Speaker *speaker, size_t buffer_size) {
  this->buffer_size_ = buffer_size;
  this->allocate_buffer_();
  this->speaker_ = speaker;
  return allocated_successfully();
}

bool AudioSourceTransferBuffer::add_source(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size) {
  return this->add_ring_buffer_(ring_buffer, buffer_size);
}

size_t AudioSourceTransferBuffer::transfer_data_from_source(TickType_t ticks_to_wait) {
  if (!this->allocated_successfully()) {
    return 0;
  }

  // Shift data in buffer to start
  if (this->buffer_length_ > 0) {
    memmove(this->buffer_, this->data_start_, this->buffer_length_);
  }
  this->data_start_ = this->buffer_;

  size_t bytes_to_read = this->free();
  size_t bytes_read = 0;
  if (bytes_to_read > 0) {
    if (this->ring_buffer_.use_count() > 0) {
      bytes_read = this->ring_buffer_->read((void *) this->get_buffer_end(), bytes_to_read, ticks_to_wait);
    }

    this->increase_buffer_length(bytes_read);
  }
  return bytes_read;
}

size_t AudioSinkTransferBuffer::transfer_data_to_sink(TickType_t ticks_to_wait) {
  if (!this->allocated_successfully()) {
    return 0;
  }

  size_t bytes_written = 0;
  if (this->available()) {
    if (this->speaker_ != nullptr) {
      bytes_written = this->speaker_->play(this->data_start_, this->available(), ticks_to_wait);
    } else if (this->ring_buffer_.use_count() > 0) {
      bytes_written =
          this->ring_buffer_->write_without_replacement((void *) this->data_start_, this->available(), ticks_to_wait);
    }

    this->decrease_buffer_length(bytes_written);

    // Shift unwritten data to the start of the buffer
    memmove(this->buffer_, this->data_start_, this->buffer_length_);
    this->data_start_ = this->buffer_;
  }
  return bytes_written;
}

bool AudioSinkTransferBuffer::has_buffered_data() {
  if (this->speaker_ != nullptr) {
    return (this->speaker_->has_buffered_data() || (this->available() > 0));
  } else if (this->ring_buffer_.use_count() > 0) {
    return ((this->ring_buffer_->available() > 0) || (this->available() > 0));
  }
  return (this->available() > 0);
}

bool AudioTransferBuffer::allocated_successfully() {
  if ((this->buffer_ != nullptr) || (this->buffer_size_ == 0)) {
    return true;
  }

  return false;
}

void AudioTransferBuffer::decrease_buffer_length(size_t bytes) {
  this->buffer_length_ -= bytes;
  this->data_start_ += bytes;
}
}  // namespace audio
}  // namespace esphome
