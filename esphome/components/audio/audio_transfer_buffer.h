
#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esphome/components/speaker/speaker.h"

#include "esp_err.h"

#include <freertos/FreeRTOS.h>

namespace esphome {
namespace audio {

class AudioTransferBuffer {
  /*
   * @brief Class that facilitates tranferring data between a buffer and an audio source or sink.
   * The transfer buffer temporarily holds data for processing in other audio components.
   */
 public:
  AudioTransferBuffer() {}
  AudioTransferBuffer(size_t buffer_size) : buffer_size_(buffer_size) {}

  /// @brief Destructor that deallocates the transfer buffer
  ~AudioTransferBuffer();

  /// @brief Returns true if the transfer buffer was successfully allocated, false otherwise
  bool allocated_successfully();

  /// @brief Returns a pointer to the start of the transfer buffer where available() bytes of exisiting data can be read
  uint8_t *get_buffer_start() { return this->data_start_; }

  /// @brief Returns a pointer to the end of the transfer buffer where free() bytes of new data can be written
  uint8_t *get_buffer_end() { return this->data_start_ + this->buffer_length_; }

  /// @brief Updates the internal state of the transfer buffer. This should be called after reading data
  /// @param bytes The number of bytes consumed/read
  void decrease_buffer_length(size_t bytes);

  /// @brief Updates the internal state of the transfer buffer. This should be called after writing data
  /// @param bytes The number of bytes written
  void increase_buffer_length(size_t bytes) { this->buffer_length_ += bytes; }

  /// @brief Returns the transfer buffer's currrently free bytes that can be written
  size_t free() { return this->buffer_size_ - (this->buffer_length_ - (this->data_start_ - this->buffer_)); }

  /// @brief Returns the transfer buffer's currently available bytes that can be read
  size_t available() { return this->buffer_length_; }

  /// @brief Returns the transfer buffers allocated bytes
  size_t capacity() { return this->buffer_size_; }

  /// @brief Tests if there is any data in the tranfer buffer or the source/sink.
  /// @return True if there is data, false otherwise.
  virtual bool has_buffered_data();

  /// @brief Clears data in the transfer buffer and, if possible, the source/sink.
  void clear_buffered_data();

 protected:
  /// @brief Adds a ring buffer as the transfer buffer's source/sink.
  /// @param ring_buffer weak_ptr to the allocated ring buffer
  /// @param buffer_size the size of the transfer buffer
  /// @return True if the transfer buffer is allocated sucessfully, false otherwise
  bool add_ring_buffer_(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size);

  /// @brief Allocates the transfer buffer in external memory, if available.
  void allocate_buffer_();

  // A possible source or sink for the transfer buffer
  std::shared_ptr<RingBuffer> ring_buffer_;

  uint8_t *buffer_{nullptr};
  uint8_t *data_start_{nullptr};

  size_t buffer_size_{0};
  size_t buffer_length_;
};

class AudioSinkTransferBuffer : public AudioTransferBuffer {
  /*
   * @brief A class that implements a transfer buffer for audio sinks.
   * Supports writing audio data to a ring buffer or a speaker component.
   */
 public:
  AudioSinkTransferBuffer() {}
  AudioSinkTransferBuffer(size_t buffer_size) : AudioTransferBuffer(buffer_size) { this->allocate_buffer_(); }

  /// @brief Writes any available data in the transfer buffer to the sink.
  /// @param ticks_to_wait FreeRTOS ticks to block while waiting for the sink to have enough space
  /// @return Number of bytes written
  size_t transfer_data_to_sink(TickType_t ticks_to_wait);

  /// @brief Adds a ring buffer as the transfer buffer's sink.
  /// @param ring_buffer weak_ptr to the allocated ring buffer
  /// @param buffer_size The size of the tranfer buffer
  /// @return True if the transfer buffer is allocated sucessfully, false otherwise
  bool add_sink(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size);

  /// @brief Adds a speaker as the transfer buffer's sink.
  /// @param speaker Pointer to the speaker component
  /// @param buffer_size The size of the tranfer buffer
  /// @return True if the transfer buffer is allocated sucessfully, false otherwise
  bool add_sink(speaker::Speaker *speaker, size_t buffer_size);

  bool has_buffered_data() override;

 protected:
  speaker::Speaker *speaker_{nullptr};
};

class AudioSourceTransferBuffer : public AudioTransferBuffer {
  /*
   * @brief A class that implements a transfer buffer for audio sources.
   * Supports reading audio data from a ring buffer.
   */
 public:
  /// @brief Reads any available data from the sink into the transfer buffer.
  /// @param ticks_to_wait FreeRTOS ticks to block while waiting for the source to have enough data
  /// @return Number of bytes read
  size_t transfer_data_from_source(TickType_t ticks_to_wait);

  /// @brief Adds a ring buffer as the transfer buffer's source.
  /// @param ring_buffer weak_ptr to the allocated ring buffer
  /// @param buffer_size The size of the tranfer buffer
  /// @return True if the transfer buffer is allocated sucessfully, false otherwise
  bool add_source(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size);
};

}  // namespace audio
}  // namespace esphome
