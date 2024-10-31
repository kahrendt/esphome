#pragma once

#ifdef USE_ESP_IDF

#include <flac_decoder.h>
#include <wav_decoder.h>
#include <mp3_decoder.h>

#include "audio.h"
#include "audio_files.h"
#include "audio_stage.h"

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

class AudioDecoder : public AudioInputStage, public AudioOutputStage {
 public:
  AudioDecoder(std::shared_ptr<RingBuffer> &input_ring_buffer, std::shared_ptr<esphome::RingBuffer> output_ring_buffer,
               size_t internal_buffer_size)
      : AudioInputStage(input_ring_buffer, internal_buffer_size),
        AudioOutputStage(output_ring_buffer, internal_buffer_size) {}
  // input_ring_buffer_(input_ring_buffer),
  // internal_buffer_size_(internal_buffer_size) {}
  ~AudioDecoder();

  esp_err_t start(AudioFileType audio_file_type);

  AudioDecoderState decode(bool stop_gracefully);

  const optional<audio::AudioStreamInfo> &get_audio_stream_info() const { return this->audio_stream_info_; }

 protected:
  esp_err_t allocate_buffers_();

  FileDecoderState decode_flac_();
  FileDecoderState decode_mp3_();
  FileDecoderState decode_wav_();

  // std::shared_ptr<RingBuffer> input_ring_buffer_;
  // size_t internal_buffer_size_;

  // uint8_t *input_buffer_{nullptr};
  // uint8_t *input_buffer_current_{nullptr};
  // size_t input_buffer_length_;

  // uint8_t *output_buffer_current_{nullptr};

  std::unique_ptr<flac::FLACDecoder> flac_decoder_;

  HMP3Decoder mp3_decoder_;

  std::unique_ptr<wav_decoder::WAVDecoder> wav_decoder_;
  size_t wav_bytes_left_;

  AudioFileType audio_file_type_{AudioFileType::NONE};
  optional<AudioStreamInfo> audio_stream_info_{};

  size_t potentially_failed_count_{0};
  bool end_of_file_{false};
};
}  // namespace audio
}  // namespace esphome

#endif
