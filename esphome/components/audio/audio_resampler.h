#pragma once

#include "audio.h"
#include "audio_transfer_buffer.h"

#include "esphome/core/ring_buffer.h"

#include "esphome/components/speaker/speaker.h"

#include <resampler.h>  // esp-audio-libs

namespace esphome {
namespace audio {

enum class AudioResamplerState : uint8_t {
  RESAMPLING,  // More data is available to resample
  FINISHED,    // All file data has been resampled and transferred
  FAILED,      // Unused state included for consistency among Audio classes
};

struct ResampleInfo {
  bool resample;        // True if converting sample rates
  bool mono_to_stereo;  // True if converting mono to stereo
};

class AudioResampler {
  /*
   * @brief Class that facilitates resampling an audio file.
   * The audio data is read from a ring buffer source, decoded, and sent to an audio sink (ring buffer or speaker
   * component).
   * Supports adjusting the sample rate and converting mono audio to stereo.
   */
 public:
  /// @brief Allocates the input and output transfer buffers
  /// @param input_buffer_size Size of the input transfer buffer in bytes.
  /// @param output_buffer_size Size of the output transfer buffer in bytes.
  AudioResampler(size_t input_buffer_size, size_t output_buffer_size);

  /// @brief Adds a source ring buffer for audio data. Takes ownership of the ring buffer in a shared_ptr.
  /// @param input_ring_buffer weak_ptr of a shared_ptr of the sink ring buffer to transfer ownership
  /// @return ESP_OK if successsful, ESP_ERR_NO_MEM if the transfer buffer wasn't allocated
  esp_err_t add_source(std::weak_ptr<RingBuffer> input_ring_buffer);

  /// @brief Adds a sink ring buffer for resampled audio. Takes ownership of the ring buffer in a shared_ptr.
  /// @param output_ring_buffer weak_ptr of a shared_ptr of the sink ring buffer to transfer ownership
  /// @return ESP_OK if successsful, ESP_ERR_NO_MEM if the transfer buffer wasn't allocated
  esp_err_t add_sink(std::weak_ptr<RingBuffer> output_ring_buffer);

#ifdef USE_SPEAKER
  /// @brief Adds a sink speaker for decoded audio.
  /// @param speaker pointer to speaker component
  /// @return ESP_OK if successsful, ESP_ERR_NO_MEM if the transfer buffer wasn't allocated
  esp_err_t add_sink(speaker::Speaker *speaker);
#endif

  /// @brief Sets up the class to resample
  /// @param stream_info the incoming sample rate, bits per sample, and number of channels
  /// @param target_sample_rate the necessary sample rate to convert to
  /// @param resample_info ResampleInfo object passed-by-reference that indicates which resampling processes are applied
  /// @return ESP_OK if it is able to convert the incoming stream, ESP_ERR_NO_MEM if the transfer buffers failed to
  /// allocate, ESP_ERR_NOT_SUPPORTED if the stream can't be converted.
  esp_err_t start(AudioStreamInfo &stream_info, uint32_t target_sample_rate, ResampleInfo &resample_info);

  /// @brief Resamples audio from the ring buffer source and writes to the sink.
  /// @param stop_gracefully If true, it indicates the file decoder is finished. The resampler will resample all the
  /// reamining audio and then finish.
  /// @return AudioResamplerState
  AudioResamplerState resample(bool stop_gracefully);

 protected:
  std::unique_ptr<AudioSourceTransferBuffer> input_transfer_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;

  size_t input_buffer_size_;
  size_t output_buffer_size_;

  AudioStreamInfo stream_info_;
  ResampleInfo resample_info_;

  std::unique_ptr<resampler::Resampler> resampler_;
};

}  // namespace audio
}  // namespace esphome
