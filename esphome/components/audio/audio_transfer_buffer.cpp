#include "audio_transfer_buffer.h"

namespace esphome {
namespace audio {
AudioTransferBuffer::~AudioTransferBuffer() {
  if (this->buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->buffer_, this->buffer_size_);
  }
}

size_t AudioInTransferBuffer::read_ring_buffer(TickType_t ticks_to_wait) {
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

size_t AudioOutTransferBuffer::transfer_audio_out(TickType_t ticks_to_wait) {
  if (!this->allocated_successfully()) {
    return 0;
  }

  size_t bytes_written = 0;
  if ((this->buffer_length_ > 0) && (this->available())) {
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

size_t AudioOutTransferBuffer::transfer_audio_out(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->speaker_ != nullptr) {
    return this->speaker_->play(data, length, ticks_to_wait);
  } else if (this->ring_buffer_.use_count() > 0) {
    return this->ring_buffer_->write_without_replacement((void *) data, length, ticks_to_wait);
  }
  return 0;
}

bool AudioTransferBuffer::allocated_successfully() {
  // if (this->ring_buffer_.use_count() && (this->buffer_ != nullptr)) {
  //   return true;
  // }

  if ((this->buffer_ != nullptr) || (this->buffer_size_ == 0)) {
    return true;
  }

  return false;
}

bool AudioOutTransferBuffer::allocated_successfully() {
  // if (this->ring_buffer_.use_count() && (this->buffer_ != nullptr)) {
  //   return true;
  // } else if ((this->speaker_ != nullptr) && (this->buffer_ != nullptr)) {
  //   return true;
  // }
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
