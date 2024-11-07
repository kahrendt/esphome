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
  INITIALIZED = 0,
  DECODING,
  FINISHED,
  FAILED,
};

// Only used within the AudioDecoder class; conveys the state of the particular file type decoder
enum class FileDecoderState : uint8_t {
  MORE_TO_PROCESS,
  IDLE,
  POTENTIALLY_FAILED,
  FAILED,
  END_OF_FILE,
};

class AudioDecoder {
 public:
  AudioDecoder(size_t input_buffer_size, size_t output_buffer_size) {
    this->input_transfer_buffer_ = AudioSourceTransferBuffer::create(input_buffer_size);
    this->output_transfer_buffer_ = AudioSinkTransferBuffer::create(output_buffer_size);
  }

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

  esp_err_t start(AudioFileType audio_file_type);

  AudioDecoderState decode(bool stop_gracefully);

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
