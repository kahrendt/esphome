#pragma once

#ifdef USE_ESP_IDF

#include <flac_decoder.h>
#include <wav_decoder.h>
#include <mp3_decoder.h>

#include "audio.h"
#include "audio_files.h"
#include "audio_transfer_buffer.h"

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

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
  // AudioDecoder(std::shared_ptr<RingBuffer> &input_ring_buffer, std::shared_ptr<esphome::RingBuffer>
  // &output_ring_buffer,
  //              size_t internal_buffer_size) {
  //   // this->input_transfer_buffer_ = make_unique<AudioInTransferBuffer>(input_ring_buffer, internal_buffer_size);
  //   // this->output_transfer_buffer_ = make_unique<AudioOutTransferBuffer>(output_ring_buffer, internal_buffer_size);
  // }
  AudioDecoder(size_t buffer_size) {
    this->input_transfer_buffer_ = make_unique<AudioInTransferBuffer>();
    this->output_transfer_buffer_ = make_unique<AudioOutTransferBuffer>(buffer_size);
  }

  ~AudioDecoder();

  bool add_input_ring_buffer(std::weak_ptr<esphome::RingBuffer> input_ring_buffer, size_t input_buffer_size) {
    // this->input_transfer_buffer_ = make_unique<AudioInTransferBuffer>();
    this->input_transfer_buffer_->add_input(input_ring_buffer, input_buffer_size);
    return true;
  }
  bool add_output_ring_buffer(std::weak_ptr<esphome::RingBuffer> output_ring_buffer, size_t output_buffer_size) {
    // this->output_transfer_buffer_ = make_unique<AudioOutTransferBuffer>();
    this->output_transfer_buffer_->add_output(output_ring_buffer, output_buffer_size);
    return true;
  }

  esp_err_t start(AudioFileType audio_file_type);

  AudioDecoderState decode(bool stop_gracefully);

  const optional<audio::AudioStreamInfo> &get_audio_stream_info() const { return this->audio_stream_info_; }

 protected:
  FileDecoderState decode_flac_();
  FileDecoderState decode_mp3_();
  FileDecoderState decode_wav_();

  std::unique_ptr<AudioInTransferBuffer> input_transfer_buffer_;
  std::unique_ptr<AudioOutTransferBuffer> output_transfer_buffer_;

  std::unique_ptr<flac::FLACDecoder> flac_decoder_;

  HMP3Decoder mp3_decoder_;

  std::unique_ptr<wav_decoder::WAVDecoder> wav_decoder_;
  size_t wav_bytes_left_{0};

  AudioFileType audio_file_type_{AudioFileType::NONE};
  optional<AudioStreamInfo> audio_stream_info_{};

  size_t free_buffer_required_;

  size_t potentially_failed_count_{0};
  bool end_of_file_{false};
};
}  // namespace audio
}  // namespace esphome

#endif
