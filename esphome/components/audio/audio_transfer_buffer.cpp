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

void AudioTransferBuffer::decrease_buffer_length(size_t bytes) {
  this->buffer_length_ -= bytes;
  this->data_start_ += bytes;
}

void AudioTransferBuffer::allocate_buffer_(size_t buffer_size) {
  this->buffer_size_ = buffer_size;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);

  this->buffer_ = allocator.allocate(this->buffer_size_);
  this->data_start_ = this->buffer_;
  this->buffer_length_ = 0;
}

size_t AudioSourceTransferBuffer::transfer_data_from_source(TickType_t ticks_to_wait) {
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
  size_t bytes_written = 0;
  if (this->available()) {
    if (this->speaker_ != nullptr) {
      bytes_written = this->speaker_->play(this->data_start_, this->available(), ticks_to_wait);
    }

    if (this->ring_buffer_.use_count() > 0) {
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

}  // namespace audio
}  // namespace esphome
