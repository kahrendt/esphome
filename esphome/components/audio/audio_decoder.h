#pragma once

#ifdef USE_ESP_IDF

#include <wav_decoder.h>
#if !defined(SIMPLE_MEDIA_PLAYER)
#include <flac_decoder.h>
#include <mp3_decoder.h>
#endif

#include "audio.h"
#include "audio_files.h"
#include "audio_transfer_buffer.h"

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#include "esp_err.h"

namespace esphome {
namespace audio {

enum class AudioDecoderState : uint8_t {
  DECODING = 0,  // More data is available to decode
  FINISHED,      // All file data has been decoded and transferred
  FAILED,        // Encountered an error
};

// Only used within the AudioDecoder class; conveys the state of the particular file type decoder
enum class FileDecoderState : uint8_t {
  MORE_TO_PROCESS,     // Successsfully read a file chunk and more data is available to decode
  IDLE,                // Not enough data to decode, waiting for more to be transferred
  POTENTIALLY_FAILED,  // Decoder encountered a potentially recoverable error if more file data is available
  FAILED,              // Decoder encoutnered an uncrecoverable error
  END_OF_FILE,         // The specific file decoder knows its the end of the file
};

class AudioDecoder {
  /*
   * @brief Class that facilitates decoding an audio file.
   * The audio file is read from a ring buffer source, decoded, and sent to an audio sink (ring buffer or speaker
   * component).
   * Supports wav, flac, and mp3 formats.
   */
 public:
  /// @brief Allocates the input and output transfer buffers
  /// @param input_buffer_size Size of the input transfer buffer in bytes.
  /// @param output_buffer_size Size of the output transfer buffer in bytes.
  AudioDecoder(size_t input_buffer_size, size_t output_buffer_size);

  /// @brief Deallocates the MP3 decoder (the flac and wav decoders are deallocated automatically)
  ~AudioDecoder();

  esp_err_t add_input_ring_buffer(std::weak_ptr<esphome::RingBuffer> input_ring_buffer) {
    if (this->input_transfer_buffer_ != nullptr) {
      this->input_transfer_buffer_->set_source(input_ring_buffer);
      return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
  }

  esp_err_t add_output_ring_buffer(std::weak_ptr<esphome::RingBuffer> output_ring_buffer) {
    if (this->output_transfer_buffer_ != nullptr) {
      this->output_transfer_buffer_->set_sink(output_ring_buffer);
      return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
  }

#ifdef USE_SPEAKER
  esp_err_t add_speaker(speaker::Speaker *speaker) {
    if (this->output_transfer_buffer_ != nullptr) {
      this->output_transfer_buffer_->set_sink(speaker);
      return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
  }
#endif

  /// @brief Sets up decoding the file
  /// @param audio_file_type AudioFileType of the file
  /// @return ESP_OK if successful, ESP_ERR_NO_MEM if the transfer buffers fail to allocate, or ESP_ERR_NOT_SUPPORTED if
  /// the format isn't supported.
  esp_err_t start(AudioFileType audio_file_type);

  /// @brief Decodes audio from the ring buffer source and writes to the sink.
  /// @param stop_gracefully If true, it indicates the file source is finished. The decoder will decode all the
  /// reamining data and then finish.
  /// @return AudioDecoderState
  AudioDecoderState decode(bool stop_gracefully);

  /// @brief Gets the audio stream information, if it has been decoded from the files header
  /// @return optional<AudioStreamInfo> with the audio information. If not available yet, returns no value.
  const optional<audio::AudioStreamInfo> &get_audio_stream_info() const { return this->audio_stream_info_; }

 protected:
  FileDecoderState decode_wav_();
  std::unique_ptr<wav_decoder::WAVDecoder> wav_decoder_;
#if !defined(SIMPLE_MEDIA_PLAYER)
  FileDecoderState decode_flac_();
  std::unique_ptr<flac::FLACDecoder> flac_decoder_;
  FileDecoderState decode_mp3_();
  HMP3Decoder mp3_decoder_;
#endif

  std::unique_ptr<AudioSourceTransferBuffer> input_transfer_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;

  size_t wav_bytes_left_{0};

  AudioFileType audio_file_type_{AudioFileType::NONE};
  optional<AudioStreamInfo> audio_stream_info_{};

  size_t free_buffer_required_;

  size_t potentially_failed_count_{0};
  bool end_of_file_{false};
  bool wav_has_known_end_{false};
};
}  // namespace audio
}  // namespace esphome

#endif
