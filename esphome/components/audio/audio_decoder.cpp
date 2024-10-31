#ifdef USE_ESP_IDF

#include "audio_decoder.h"

#include "mp3_decoder.h"

#include "esphome/core/ring_buffer.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace audio {

static const size_t READ_WRITE_TIMEOUT_MS = 20;

AudioDecoder::~AudioDecoder() {
  // ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  // if (this->input_buffer_ != nullptr) {
  //   allocator.deallocate(this->input_buffer_, this->internal_buffer_size_);
  // }

  if (this->flac_decoder_ != nullptr) {
    this->flac_decoder_->free_buffers();
    this->flac_decoder_.reset();  // Free the unique_ptr
    this->flac_decoder_ = nullptr;
  }

  if (this->audio_file_type_ == AudioFileType::MP3) {
    MP3FreeDecoder(this->mp3_decoder_);
  }

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
    case AudioFileType::FLAC:
      this->flac_decoder_ = make_unique<flac::FLACDecoder>(this->input_transfer_buffer_->get_buffer_start());
      break;
    case AudioFileType::MP3:
      this->mp3_decoder_ = MP3InitDecoder();
      break;
    case AudioFileType::WAV:
      // this->wav_decoder_ = make_unique<wav_decoder::WAVDecoder>(&this->input_transfer_buffer_->get_buffer_start());
      // this->wav_decoder_->reset();
      break;
    case AudioFileType::NONE:
      return ESP_ERR_NOT_SUPPORTED;
      break;
  }

  return ESP_OK;
}

AudioDecoderState AudioDecoder::decode(bool stop_gracefully) {
  if (stop_gracefully) {
    if (this->output_transfer_buffer_->available() == 0) {
      // If the file decoder believes it the end of file
      if (this->end_of_file_) {
        return AudioDecoderState::FINISHED;
      }
      // If all the internal buffers are empty, the decoding is done
      if ((this->input_transfer_buffer_->get_ring_buffer()->available() == 0) &&
          (this->input_transfer_buffer_->available() == 0)) {
        return AudioDecoderState::FINISHED;
      }

      // If the ring buffer has no new data and the decoding failed last time, mark done
      if ((this->input_transfer_buffer_->get_ring_buffer()->available() == 0) &&
          (this->potentially_failed_count_ > 0)) {
        return AudioDecoderState::FINISHED;
      }
    }
  }

  if (this->potentially_failed_count_ > 10) {
    return AudioDecoderState::FAILED;
  }

  FileDecoderState state = FileDecoderState::MORE_TO_PROCESS;

  while (state == FileDecoderState::MORE_TO_PROCESS) {
    if (this->output_transfer_buffer_->available() > 0) {
      this->output_transfer_buffer_->write_ring_buffer(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
      return AudioDecoderState::DECODING;
    } else {
      // Decode more data
      size_t bytes_read = this->input_transfer_buffer_->read_ring_buffer(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));

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
          case AudioFileType::FLAC:
            state = this->decode_flac_();
            break;
          case AudioFileType::MP3:
            state = this->decode_mp3_();
            break;
          case AudioFileType::WAV:
            state = this->decode_wav_();
            break;
          case AudioFileType::NONE:
            state = FileDecoderState::IDLE;
            break;
        }
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

FileDecoderState AudioDecoder::decode_flac_() {
  if (!this->audio_stream_info_.has_value()) {
    // Header hasn't been read
    auto result = this->flac_decoder_->read_header(this->input_transfer_buffer_->available());

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

    audio::AudioStreamInfo audio_stream_info;
    audio_stream_info.channels = this->flac_decoder_->get_num_channels();
    audio_stream_info.sample_rate = this->flac_decoder_->get_sample_rate();
    audio_stream_info.bits_per_sample = this->flac_decoder_->get_sample_depth();

    this->audio_stream_info_ = audio_stream_info;

    return FileDecoderState::MORE_TO_PROCESS;
  }

  uint32_t output_samples = 0;
  auto result =
      this->flac_decoder_->decode_frame(this->input_transfer_buffer_->available(),
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
  // this->input_buffer_current_ += offset;
  // this->input_buffer_length_ -= offset;

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
      // this->output_buffer_length_ = mp3_frame_info.outputSamps * bytes_per_sample;
      // this->output_buffer_current_ = this->output_buffer_;
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

FileDecoderState AudioDecoder::decode_wav_() {
  // if (!this->audio_stream_info_.has_value() && (this->input_buffer_length_ > 44)) {
  //   // Header hasn't been processed

  //   size_t original_buffer_length = this->input_buffer_length_;

  //   size_t wav_bytes_to_skip = this->wav_decoder_->bytes_to_skip();
  //   size_t wav_bytes_to_read = this->wav_decoder_->bytes_needed();

  //   bool header_finished = false;
  //   while (!header_finished) {
  //     if (wav_bytes_to_skip > 0) {
  //       // Adjust pointer to skip the appropriate bytes
  //       this->input_buffer_current_ += wav_bytes_to_skip;
  //       this->input_buffer_length_ -= wav_bytes_to_skip;
  //       wav_bytes_to_skip = 0;
  //     } else if (wav_bytes_to_read > 0) {
  //       wav_decoder::WAVDecoderResult result = this->wav_decoder_->next();
  //       this->input_buffer_current_ += wav_bytes_to_read;
  //       this->input_buffer_length_ -= wav_bytes_to_read;

  //       if (result == wav_decoder::WAV_DECODER_SUCCESS_IN_DATA) {
  //         // Header parsing is complete

  //         // Assume PCM
  //         audio::AudioStreamInfo audio_stream_info;
  //         audio_stream_info.channels = this->wav_decoder_->num_channels();
  //         audio_stream_info.sample_rate = this->wav_decoder_->sample_rate();
  //         audio_stream_info.bits_per_sample = this->wav_decoder_->bits_per_sample();
  //         this->audio_stream_info_ = audio_stream_info;
  //         this->wav_bytes_left_ = this->wav_decoder_->chunk_bytes_left();
  //         header_finished = true;
  //       } else if (result == wav_decoder::WAV_DECODER_SUCCESS_NEXT) {
  //         // Continue parsing header
  //         wav_bytes_to_skip = this->wav_decoder_->bytes_to_skip();
  //         wav_bytes_to_read = this->wav_decoder_->bytes_needed();
  //       } else {
  //         // Unexpected error parsing the wav header
  //         return FileDecoderState::FAILED;
  //       }
  //     } else {
  //       // Something unexpected has happened
  //       // Reset state and hope we have enough info next time
  //       this->input_buffer_length_ = original_buffer_length;
  //       this->input_buffer_current_ = this->input_buffer_;
  //       return FileDecoderState::POTENTIALLY_FAILED;
  //     }
  //   }
  // }

  // if (this->wav_bytes_left_ > 0) {
  //   size_t bytes_to_write = std::min(this->wav_bytes_left_, this->input_buffer_length_);
  //   bytes_to_write = std::min(bytes_to_write, this->output_buffer_size_);
  //   if (bytes_to_write > 0) {
  //     std::memcpy(this->output_buffer_, this->input_buffer_current_, bytes_to_write);
  //     this->input_buffer_current_ += bytes_to_write;
  //     this->input_buffer_length_ -= bytes_to_write;
  //     // this->output_buffer_current_ = this->output_buffer_;
  //     this->output_buffer_length_ = bytes_to_write;
  //     this->wav_bytes_left_ -= bytes_to_write;
  //   }

  //   return FileDecoderState::IDLE;
  // }

  return FileDecoderState::END_OF_FILE;
}

}  // namespace audio
}  // namespace esphome

#endif
