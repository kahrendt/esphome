#include "audio_transfer_buffer.h"

namespace esphome {
namespace audio {
AudioTransferBuffer::~AudioTransferBuffer() {
  if (this->buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->buffer_, this->buffer_size_);
  }
}

size_t AudioTransferBuffer::read_ring_buffer(TickType_t ticks_to_wait) {
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
    bytes_read = this->ring_buffer_->read((void *) this->get_buffer_end(), bytes_to_read, ticks_to_wait);
    this->increase_buffer_length(bytes_read);
  }
  return bytes_read;
}

size_t AudioTransferBuffer::write_ring_buffer(TickType_t ticks_to_wait) {
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

bool AudioTransferBuffer::allocated_successfully() {
  if (this->ring_buffer_.use_count() && (this->buffer_ != nullptr)) {
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
