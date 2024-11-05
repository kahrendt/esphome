#ifdef USE_ESP_IDF

#include "audio_decoder.h"

#if !defined(SIMPLE_MEDIA_PLAYER)
#include "mp3_decoder.h"
#endif

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace audio {

static const size_t READ_WRITE_TIMEOUT_MS = 20;
static const size_t DECODING_TIMEOUT_MS = 50;

AudioDecoder::~AudioDecoder() {
#if !defined(SIMPLE_MEDIA_PLAYER)
  if (this->flac_decoder_ != nullptr) {
    this->flac_decoder_->free_buffers();
    this->flac_decoder_.reset();  // Free the unique_ptr
    this->flac_decoder_ = nullptr;
  }

  if (this->audio_file_type_ == AudioFileType::MP3) {
    MP3FreeDecoder(this->mp3_decoder_);
  }
#endif
  if (this->wav_decoder_ != nullptr) {
    this->wav_decoder_.reset();  // Free the unique_ptr
    this->wav_decoder_ = nullptr;
  }
}

esp_err_t AudioDecoder::start(AudioFileType audio_file_type) {
  if (!this->input_transfer_buffer_->allocated_successfully() ||
      !this->output_transfer_buffer_->allocated_successfully()) {
    return ESP_ERR_NO_MEM;
  }

  this->audio_file_type_ = audio_file_type;

  this->potentially_failed_count_ = 0;
  this->end_of_file_ = false;

  switch (this->audio_file_type_) {
#if !defined(SIMPLE_MEDIA_PLAYER)
    case AudioFileType::FLAC:
      this->flac_decoder_ = make_unique<flac::FLACDecoder>();
      this->free_buffer_required_ =
          this->output_transfer_buffer_->capacity();  // We'll revise this after reading the header
      break;
    case AudioFileType::MP3:
      this->mp3_decoder_ = MP3InitDecoder();
      this->free_buffer_required_ = 1152 * sizeof(int16_t) * 2;  // samples * size per sample * channels
      break;
#endif
    case AudioFileType::WAV:
      this->wav_decoder_ = make_unique<wav_decoder::WAVDecoder>();
      this->wav_decoder_->reset();
      this->free_buffer_required_ = 1024;
      break;
    case AudioFileType::NONE:
    default:
      return ESP_ERR_NOT_SUPPORTED;
      break;
  }

  return ESP_OK;
}

AudioDecoderState AudioDecoder::decode(bool stop_gracefully) {
  if (stop_gracefully) {
    if (!this->output_transfer_buffer_->has_buffered_data()) {
      if (this->end_of_file_) {
        // the file decoder indicates it reached the end of file
        return AudioDecoderState::FINISHED;
      }

      if (!this->input_transfer_buffer_->has_buffered_data()) {
        // If all the internal buffers are empty, the decoding is done
        return AudioDecoderState::FINISHED;
      }
    }
  }

  if (this->potentially_failed_count_ > 10) {
    if (stop_gracefully) {
      // No more new data is going to come in, so decoding is done
      return AudioDecoderState::FINISHED;
    }
    return AudioDecoderState::FAILED;
  }

  FileDecoderState state = FileDecoderState::MORE_TO_PROCESS;

  uint32_t decoding_start = millis();

  while (state == FileDecoderState::MORE_TO_PROCESS) {
    // Transfer decoded out
    if (this->output_transfer_buffer_->available() > 0) {
      this->output_transfer_buffer_->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
    }

    // Verify there is enough space to store more decoded audio and that the function hasn't been running too long
    if ((this->output_transfer_buffer_->free() < this->free_buffer_required_) ||
        (millis() - decoding_start > DECODING_TIMEOUT_MS)) {
      return AudioDecoderState::DECODING;
    }

    // Decode more data
    size_t bytes_read = this->input_transfer_buffer_->transfer_data_from_source(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

    if ((this->potentially_failed_count_ > 0) && (bytes_read == 0)) {
      // Failed to decode in last attempt and there is no new data

      if (this->input_transfer_buffer_->free() == 0) {
        // The input buffer is full. Since it previously failed on the exact same data, we can never recover
        state = FileDecoderState::FAILED;
      } else {
        // Attempt to get more data next time
        state = FileDecoderState::IDLE;
      }
    } else if (this->input_transfer_buffer_->available() == 0) {
      // No data to decode, attempt to get more data next time
      state = FileDecoderState::IDLE;
    } else {
      switch (this->audio_file_type_) {
#if !defined(SIMPLE_MEDIA_PLAYER)
        case AudioFileType::FLAC:
          state = this->decode_flac_();
          break;
        case AudioFileType::MP3:
          state = this->decode_mp3_();
          break;
#endif
        case AudioFileType::WAV:
          state = this->decode_wav_();
          break;
        case AudioFileType::NONE:
        default:
          state = FileDecoderState::IDLE;
          break;
      }
    }

    if (state == FileDecoderState::POTENTIALLY_FAILED) {
      ++this->potentially_failed_count_;
    } else if (state == FileDecoderState::END_OF_FILE) {
      this->end_of_file_ = true;
    } else if (state == FileDecoderState::FAILED) {
      return AudioDecoderState::FAILED;
    } else if (state == FileDecoderState::MORE_TO_PROCESS) {
      this->potentially_failed_count_ = 0;
    }
  }
  return AudioDecoderState::DECODING;
}

#if !defined(SIMPLE_MEDIA_PLAYER)
FileDecoderState AudioDecoder::decode_flac_() {
  if (!this->audio_stream_info_.has_value()) {
    // Header hasn't been read
    auto result = this->flac_decoder_->read_header(this->input_transfer_buffer_->get_buffer_start(),
                                                   this->input_transfer_buffer_->available());

    if (result == flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
      return FileDecoderState::POTENTIALLY_FAILED;
    }

    if (result != flac::FLAC_DECODER_SUCCESS) {
      // Couldn't read FLAC header
      return FileDecoderState::FAILED;
    }

    size_t bytes_consumed = this->flac_decoder_->get_bytes_index();
    this->input_transfer_buffer_->decrease_buffer_length(bytes_consumed);

    size_t flac_decoder_output_buffer_min_size = flac_decoder_->get_output_buffer_size();
    if (this->output_transfer_buffer_->capacity() < flac_decoder_output_buffer_min_size * sizeof(int16_t)) {
      // Output buffer is not big enough
      return FileDecoderState::FAILED;
    }
    this->free_buffer_required_ = flac_decoder_output_buffer_min_size * sizeof(int16_t);

    audio::AudioStreamInfo audio_stream_info;
    audio_stream_info.channels = this->flac_decoder_->get_num_channels();
    audio_stream_info.sample_rate = this->flac_decoder_->get_sample_rate();
    audio_stream_info.bits_per_sample = this->flac_decoder_->get_sample_depth();

    this->audio_stream_info_ = audio_stream_info;

    return FileDecoderState::MORE_TO_PROCESS;
  }

  uint32_t output_samples = 0;
  auto result = this->flac_decoder_->decode_frame(
      this->input_transfer_buffer_->get_buffer_start(), this->input_transfer_buffer_->available(),
      (int16_t *) this->output_transfer_buffer_->get_buffer_end(), &output_samples);

  if (result == flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
    // Not an issue, just needs more data that we'll get next time.
    return FileDecoderState::POTENTIALLY_FAILED;
  } else if (result > flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
    // Corrupted frame, don't retry with current buffer content, wait for new sync
    size_t bytes_consumed = this->flac_decoder_->get_bytes_index();
    this->input_transfer_buffer_->decrease_buffer_length(bytes_consumed);

    return FileDecoderState::POTENTIALLY_FAILED;
  }

  // We have successfully decoded some input data and have new output data
  size_t bytes_consumed = this->flac_decoder_->get_bytes_index();
  this->input_transfer_buffer_->decrease_buffer_length(bytes_consumed);
  this->output_transfer_buffer_->increase_buffer_length(output_samples * sizeof(int16_t));

  if (result == flac::FLAC_DECODER_NO_MORE_FRAMES) {
    return FileDecoderState::END_OF_FILE;
  }

  return FileDecoderState::IDLE;
}

FileDecoderState AudioDecoder::decode_mp3_() {
  // Look for the next sync word
  int32_t offset =
      MP3FindSyncWord(this->input_transfer_buffer_->get_buffer_start(), this->input_transfer_buffer_->available());
  if (offset < 0) {
    // We may recover if we have more data
    return FileDecoderState::POTENTIALLY_FAILED;
  }

  // Advance read pointer
  this->input_transfer_buffer_->increase_buffer_length(offset);

  uint8_t *buffer_start = this->input_transfer_buffer_->get_buffer_start();
  int buffer_length = (int) this->input_transfer_buffer_->available();
  int err = MP3Decode(this->mp3_decoder_, &buffer_start, &buffer_length,
                      (int16_t *) this->output_transfer_buffer_->get_buffer_end(), 0);
  if (err) {
    switch (err) {
      case ERR_MP3_MAINDATA_UNDERFLOW:
        // Not a problem. Next call to decode will provide more data.
        return FileDecoderState::POTENTIALLY_FAILED;
        break;
      default:
        return FileDecoderState::FAILED;
        break;
    }
  } else {
    size_t consumed = this->input_transfer_buffer_->available() - buffer_length;
    this->input_transfer_buffer_->decrease_buffer_length(consumed);

    MP3FrameInfo mp3_frame_info;
    MP3GetLastFrameInfo(this->mp3_decoder_, &mp3_frame_info);
    if (mp3_frame_info.outputSamps > 0) {
      int bytes_per_sample = (mp3_frame_info.bitsPerSample / 8);
      this->output_transfer_buffer_->increase_buffer_length(mp3_frame_info.outputSamps * bytes_per_sample);

      audio::AudioStreamInfo stream_info;
      stream_info.channels = mp3_frame_info.nChans;
      stream_info.sample_rate = mp3_frame_info.samprate;
      stream_info.bits_per_sample = mp3_frame_info.bitsPerSample;
      this->audio_stream_info_ = stream_info;
    }
  }

  return FileDecoderState::MORE_TO_PROCESS;
}
#endif

FileDecoderState AudioDecoder::decode_wav_() {
  if (!this->audio_stream_info_.has_value()) {
    // Header hasn't been processed

    wav_decoder::WAVDecoderResult result = this->wav_decoder_->decode_header(
        this->input_transfer_buffer_->get_buffer_start(), this->input_transfer_buffer_->available());

    if (result == wav_decoder::WAV_DECODER_SUCCESS_IN_DATA) {
      this->input_transfer_buffer_->decrease_buffer_length(this->wav_decoder_->bytes_processed());

      audio::AudioStreamInfo audio_stream_info;
      audio_stream_info.channels = this->wav_decoder_->num_channels();
      audio_stream_info.sample_rate = this->wav_decoder_->sample_rate();
      audio_stream_info.bits_per_sample = this->wav_decoder_->bits_per_sample();
      this->audio_stream_info_ = audio_stream_info;
      this->wav_bytes_left_ = this->wav_decoder_->chunk_bytes_left();
      if (this->wav_bytes_left_ > 0) {
        this->wav_has_known_end_ = true;
      } else {
        this->wav_has_known_end_ = false;
      }
    } else if (result == wav_decoder::WAV_DECODER_WARNING_INCOMPLETE_DATA) {
      // Available data didn't have the full header
      return FileDecoderState::POTENTIALLY_FAILED;
    } else {
      return FileDecoderState::FAILED;
    }
  } else {
    if (!this->wav_has_known_end_ || (this->wav_bytes_left_ > 0)) {
      size_t bytes_to_copy = this->input_transfer_buffer_->available();

      if (this->wav_has_known_end_) {
        bytes_to_copy = std::min(bytes_to_copy, this->wav_bytes_left_);
      }

      bytes_to_copy = std::min(bytes_to_copy, this->output_transfer_buffer_->free());

      if (bytes_to_copy > 0) {
        std::memcpy(this->output_transfer_buffer_->get_buffer_end(), this->input_transfer_buffer_->get_buffer_start(),
                    bytes_to_copy);
        this->input_transfer_buffer_->decrease_buffer_length(bytes_to_copy);
        this->output_transfer_buffer_->increase_buffer_length(bytes_to_copy);
        if (this->wav_has_known_end_) {
          this->wav_bytes_left_ -= bytes_to_copy;
        }
      }
      return FileDecoderState::IDLE;
    }
  }

  return FileDecoderState::END_OF_FILE;
}

}  // namespace audio
}  // namespace esphome

#endif
