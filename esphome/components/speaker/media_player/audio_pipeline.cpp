#include "audio_pipeline.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace speaker {

// static const size_t FILE_BUFFER_SIZE = 32 * 1024;
// static const size_t FILE_RING_BUFFER_SIZE = 64 * 1024;
// static const size_t BUFFER_SIZE_SAMPLES = 32768;
// static const size_t BUFFER_SIZE_BYTES = BUFFER_SIZE_SAMPLES * sizeof(int16_t);
// static const size_t FILE_BUFFER_SIZE = 4 * 1024;
// static const size_t FILE_RING_BUFFER_SIZE = 4 * 1024;
// static const size_t BUFFER_SIZE_SAMPLES = 2048;
// static const size_t BUFFER_SIZE_BYTES = BUFFER_SIZE_SAMPLES * sizeof(int16_t);

static const size_t TRANSFER_BUFFER_SIZE = 24 * 1024;
static const uint32_t DECODED_BUFFER_DURATION_MS = 500;

static const uint32_t READER_TASK_STACK_SIZE = 5 * 1024;
static const uint32_t DECODER_TASK_STACK_SIZE = 3 * 1024;
#if !defined(SIMPLE_MEDIA_PLAYER)
static const uint32_t RESAMPLER_TASK_STACK_SIZE = 3 * 1024;
#endif

static const size_t INFO_ERROR_QUEUE_COUNT = 5;

static const char *const TAG = "speaker_media_player.pipeline";

enum EventGroupBits : uint32_t {
  // The stop() function clears all unfinished bits
  // MESSAGE_* bits are only set by their respective tasks

  // Stops all activity in the pipeline elements and set by stop() or by each task
  PIPELINE_COMMAND_STOP = (1 << 0),

  // Read audio from an HTTP source; cleared by reader task and set by start(uri,...)
  READER_COMMAND_INIT_HTTP = (1 << 4),
  // Read audio from an audio file from the flash; cleared by reader task and set by start(audio_file,...)
  READER_COMMAND_INIT_FILE = (1 << 5),

  // Audio file type is read after checking it is supported; cleared by decoder task
  READER_MESSAGE_LOADED_MEDIA_TYPE = (1 << 6),
  // Reader is done (either through a failure or just end of the stream); cleared by reader task
  READER_MESSAGE_FINISHED = (1 << 7),
  // Error reading the file; cleared by get_state()
  READER_MESSAGE_ERROR = (1 << 8),

  // Decoder has determined the stream information; cleared by resampler
  DECODER_MESSAGE_LOADED_STREAM_INFO = (1 << 11),
  // Decoder is done (either through a faiilure or the end of the stream); cleared by decoder task
  DECODER_MESSAGE_FINISHED = (1 << 12),
  // Error decoding the file; cleared by get_state() by decoder task
  DECODER_MESSAGE_ERROR = (1 << 13),

#if !defined(SIMPLE_MEDIA_PLAYER)
  // Resampler is done (either through a failure or the end of the stream); cleared by resampler task
  RESAMPLER_MESSAGE_FINISHED = (1 << 17),
  // Error resampling the file; cleared by get_state()
  RESAMPLER_MESSAGE_ERROR = (1 << 18),
#endif

  // Cleared by respective tasks
  FINISHED_BITS = READER_MESSAGE_FINISHED | DECODER_MESSAGE_FINISHED
#if !defined(SIMPLE_MEDIA_PLAYER)
                  | RESAMPLER_MESSAGE_FINISHED
#endif
  ,
  UNFINISHED_BITS = ~(FINISHED_BITS | 0xff000000),  // Only 24 bits are valid for the event group, so make sure first 8
                                                    // bits of uint32 are not set; cleared by stop()
};

esp_err_t AudioPipeline::start(const std::string &uri, uint32_t target_sample_rate, const std::string &task_name,
                               UBaseType_t priority) {
  esp_err_t err = this->common_start_(target_sample_rate, task_name, priority);

  if (err == ESP_OK) {
    this->current_uri_ = uri;
    xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_HTTP);
  }

  return err;
}

esp_err_t AudioPipeline::start(audio::AudioFile *audio_file, uint32_t target_sample_rate, const std::string &task_name,
                               UBaseType_t priority) {
  esp_err_t err = this->common_start_(target_sample_rate, task_name, priority);

  if (err == ESP_OK) {
    this->current_audio_file_ = audio_file;
    xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_FILE);
  }

  return err;
}

esp_err_t AudioPipeline::allocate_buffers_() {
  if (this->event_group_ == nullptr)
    this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (this->info_error_queue_ == nullptr)
    this->info_error_queue_ = xQueueCreate(INFO_ERROR_QUEUE_COUNT, sizeof(InfoErrorEvent));

  if (this->info_error_queue_ == nullptr)
    return ESP_ERR_NO_MEM;

  return ESP_OK;
}

esp_err_t AudioPipeline::common_start_(uint32_t target_sample_rate, const std::string &task_name,
                                       UBaseType_t priority) {
  esp_err_t err = this->allocate_buffers_();
  if (err != ESP_OK) {
    return err;
  }
  err = this->stop();
  if (err != ESP_OK) {
    return err;
  }

  if (this->read_task_handle_ == nullptr) {
    xTaskCreate(AudioPipeline::read_task, (task_name + "_read").c_str(), READER_TASK_STACK_SIZE, (void *) this,
                priority, &this->read_task_handle_);
  }
  if (this->decode_task_handle_ == nullptr) {
    xTaskCreate(AudioPipeline::decode_task, (task_name + "_decode").c_str(), DECODER_TASK_STACK_SIZE, (void *) this,
                priority, &this->decode_task_handle_);
  }
#if !defined(SIMPLE_MEDIA_PLAYER)
  if (this->resample_task_handle_ == nullptr) {
    xTaskCreate(AudioPipeline::resample_task, (task_name + "_resample").c_str(), RESAMPLER_TASK_STACK_SIZE,
                (void *) this, priority, &this->resample_task_handle_);
  }
#endif

  if ((this->read_task_handle_ == nullptr) || (this->decode_task_handle_ == nullptr)
#if !defined(SIMPLE_MEDIA_PLAYER)
      || (this->resample_task_handle_ == nullptr)
#endif
  ) {
    return ESP_FAIL;
  }

  this->target_sample_rate_ = target_sample_rate;
  this->playback_ms_ = 0;

  return err;
}

AudioPipelineState AudioPipeline::get_state() {
  InfoErrorEvent event;
  if (this->info_error_queue_ != nullptr) {
    while (xQueueReceive(this->info_error_queue_, &event, 0)) {
      switch (event.source) {
        case InfoErrorSource::READER:
          if (event.err.has_value()) {
            ESP_LOGE(TAG, "Media reader encountered an error: %s", esp_err_to_name(event.err.value()));
          } else if (event.file_type.has_value()) {
            ESP_LOGD(TAG, "Reading %s file type", audio_file_type_to_string(event.file_type.value()));
          }

          break;
        case InfoErrorSource::DECODER:
          if (event.err.has_value()) {
            ESP_LOGE(TAG, "Decoder encountered an error: %s", esp_err_to_name(event.err.value()));
          }

          if (event.audio_stream_info.has_value()) {
            ESP_LOGD(TAG, "Decoded audio has %d channels, %" PRId32 " Hz sample rate, and %d bits per sample",
                     event.audio_stream_info.value().channels, event.audio_stream_info.value().sample_rate,
                     event.audio_stream_info.value().bits_per_sample);
          }

          if (event.decoding_err.has_value()) {
            switch (event.decoding_err.value()) {
              case DecodingError::FAILED_HEADER:
                ESP_LOGE(TAG, "Failed to parse the file's header.");
                break;
              case DecodingError::INCOMPATIBLE_BITS_PER_SAMPLE:
                ESP_LOGE(TAG, "Incompatible bits per sample. Only 16 bits per sample is supported");
                break;
              case DecodingError::INCOMPATIBLE_CHANNELS:
                ESP_LOGE(TAG, "Incompatible number of channels. Only 1 or 2 channel audio is supported.");
                break;
            }
          }
          break;
#if !defined(SIMPLE_MEDIA_PLAYER)
        case InfoErrorSource::RESAMPLER:
          if (event.err.has_value()) {
            ESP_LOGE(TAG, "Resampler encountered an error: %s", esp_err_to_name(event.err.has_value()));
          } else if (event.resample_info.has_value()) {
            if (event.resample_info.value().resample) {
              ESP_LOGD(TAG, "Converting the audio sample rate");
            }
            if (event.resample_info.value().mono_to_stereo) {
              ESP_LOGD(TAG, "Converting mono channel audio to stereo channel audio");
            }
          }
          break;
#endif
      }
    }
  }

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if (!this->read_task_handle_ && !this->decode_task_handle_
#if !defined(SIMPLE_MEDIA_PLAYER)
      && !this->resample_task_handle_
#endif
  ) {
    return AudioPipelineState::STOPPED;
  }

  if ((event_bits & READER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, READER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_READING;
  }

  if ((event_bits & DECODER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, DECODER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_DECODING;
  }

#if !defined(SIMPLE_MEDIA_PLAYER)
  if ((event_bits & RESAMPLER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, RESAMPLER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_RESAMPLING;
  }
#endif

  if ((event_bits & READER_MESSAGE_FINISHED) && (event_bits & DECODER_MESSAGE_FINISHED)
#if !defined(SIMPLE_MEDIA_PLAYER)
      && (event_bits & RESAMPLER_MESSAGE_FINISHED)
#endif
  ) {
    return AudioPipelineState::STOPPED;
  }

  return AudioPipelineState::PLAYING;
}  // namespace speaker

esp_err_t AudioPipeline::stop() {
  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  EventBits_t finished_bits_to_check = 0;

  if ((this->read_task_handle_ != nullptr) && !(event_bits & READER_MESSAGE_FINISHED)) {
    // read task is active
    finished_bits_to_check |= READER_MESSAGE_FINISHED;
  }

  if ((this->decode_task_handle_ != nullptr) && !(event_bits & DECODER_MESSAGE_FINISHED)) {
    // decode task is active
    finished_bits_to_check |= DECODER_MESSAGE_FINISHED;
  }

#if !defined(SIMPLE_MEDIA_PLAYER)
  if ((this->resample_task_handle_ != nullptr) && !(event_bits & RESAMPLER_MESSAGE_FINISHED)) {
    // resampler task is active
    finished_bits_to_check |= RESAMPLER_MESSAGE_FINISHED;
  }
#endif

  if (finished_bits_to_check) {
    printf("sending stop comamnd\n");
    xEventGroupSetBits(this->event_group_, PIPELINE_COMMAND_STOP);
    uint32_t event_group_bits = xEventGroupWaitBits(this->event_group_,
                                                    finished_bits_to_check,  // Bit message to read
                                                    pdFALSE,                 // Clear the bits on exit
                                                    pdTRUE,                  // Wait for all the bits,
                                                    pdMS_TO_TICKS(300));     // Duration to block/wait

    if ((event_group_bits & finished_bits_to_check) != finished_bits_to_check) {
      // Not all bits were set, so it timed out
      return ESP_ERR_TIMEOUT;
    }
  }

  // Tasks can't be running
  this->read_task_handle_ = nullptr;
  this->decode_task_handle_ = nullptr;
#if !defined(SIMPLE_MEDIA_PLAYER)
  this->resample_task_handle_ = nullptr;
#endif

  xEventGroupClearBits(this->event_group_, UNFINISHED_BITS);

  this->speaker_->stop();

  return ESP_OK;
}

void AudioPipeline::suspend_tasks() {
  if (this->read_task_handle_ != nullptr) {
    vTaskSuspend(this->read_task_handle_);
  }
  if (this->decode_task_handle_ != nullptr) {
    vTaskSuspend(this->decode_task_handle_);
  }
#if !defined(SIMPLE_MEDIA_PLAYER)
  if (this->resample_task_handle_ != nullptr) {
    vTaskSuspend(this->resample_task_handle_);
  }
#endif
}

void AudioPipeline::resume_tasks() {
  if (this->read_task_handle_ != nullptr) {
    vTaskResume(this->read_task_handle_);
  }
  if (this->decode_task_handle_ != nullptr) {
    vTaskResume(this->decode_task_handle_);
  }
#if !defined(SIMPLE_MEDIA_PLAYER)
  if (this->resample_task_handle_ != nullptr) {
    vTaskResume(this->resample_task_handle_);
  }
#endif
}

void AudioPipeline::read_task(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  // Wait until the pipeline notifies us the source of the media file
  EventBits_t event_bits = xEventGroupWaitBits(
      this_pipeline->event_group_,
      READER_COMMAND_INIT_FILE | READER_COMMAND_INIT_HTTP | PIPELINE_COMMAND_STOP,  // Bit message to read
      pdFALSE,                                                                      // Clear the bit on exit
      pdFALSE,                                                                      // Wait for all the bits,
      portMAX_DELAY);  // Block indefinitely until bit is set

  if (!(event_bits & PIPELINE_COMMAND_STOP)) {
    xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_FINISHED |
                                                          EventGroupBits::READER_COMMAND_INIT_FILE |
                                                          EventGroupBits::READER_COMMAND_INIT_HTTP);
    InfoErrorEvent event;
    event.source = InfoErrorSource::READER;
    esp_err_t err = ESP_OK;

    std::unique_ptr<audio::AudioReader> reader = make_unique<audio::AudioReader>(TRANSFER_BUFFER_SIZE);

    if (event_bits & READER_COMMAND_INIT_FILE) {
      err = reader->start(this_pipeline->current_audio_file_, this_pipeline->current_audio_file_type_);
    } else {
      err = reader->start(this_pipeline->current_uri_, this_pipeline->current_audio_file_type_);
    }

    if (err == ESP_OK) {
      std::shared_ptr<RingBuffer> temp_ring_buffer;

      size_t file_ring_buffer_size = TRANSFER_BUFFER_SIZE * this_pipeline->target_sample_rate_ / 1000;

      switch (this_pipeline->current_audio_file_type_) {
        case audio::AudioFileType::MP3:
          file_ring_buffer_size /= 8;
          break;
        case audio::AudioFileType::FLAC:
          file_ring_buffer_size /= 2;
          break;
        default:
          break;
      }

      if (!this_pipeline->raw_file_ring_buffer_.use_count()) {
        temp_ring_buffer = std::move(RingBuffer::create(file_ring_buffer_size));
        this_pipeline->raw_file_ring_buffer_ = temp_ring_buffer;
      }

      if (!this_pipeline->raw_file_ring_buffer_.use_count()) {
        err = ESP_ERR_NO_MEM;
      } else {
        reader->add_sink(this_pipeline->raw_file_ring_buffer_);
      }
    }

    if (err != ESP_OK) {
      // Send specific error message
      event.err = err;
      xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);

      // Setting up the reader failed, stop the pipeline
      xEventGroupSetBits(this_pipeline->event_group_,
                         EventGroupBits::READER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
    } else {
      // Send the file type to the pipeline
      event.file_type = this_pipeline->current_audio_file_type_;
      xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);

      // Inform the decoder that the media type is available
      xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_LOADED_MEDIA_TYPE);
    }

    while (true) {
      event_bits = xEventGroupGetBits(this_pipeline->event_group_);

      if (event_bits & PIPELINE_COMMAND_STOP) {
        break;
      }

      audio::AudioReaderState reader_state = reader->read();

      if (reader_state == audio::AudioReaderState::FINISHED) {
        break;
      } else if (reader_state == audio::AudioReaderState::FAILED) {
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::READER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        break;
      }
    }
  }

  xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_FINISHED);
  vTaskDelete(NULL);
}

void AudioPipeline::decode_task(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  // Wait until the reader notifies us that the media type is available
  EventBits_t event_bits =
      xEventGroupWaitBits(this_pipeline->event_group_,
                          READER_MESSAGE_LOADED_MEDIA_TYPE | PIPELINE_COMMAND_STOP,  // Bit message to read
                          pdFALSE,                                                   // Clear the bit on exit
                          pdFALSE,                                                   // Wait for all the bits,
                          portMAX_DELAY);  // Block indefinitely until bit is set

  if (!(event_bits & PIPELINE_COMMAND_STOP)) {
    xEventGroupClearBits(this_pipeline->event_group_,
                         EventGroupBits::DECODER_MESSAGE_FINISHED | EventGroupBits::READER_MESSAGE_LOADED_MEDIA_TYPE);
    InfoErrorEvent event;
    event.source = InfoErrorSource::DECODER;

    std::unique_ptr<audio::AudioDecoder> decoder =
        make_unique<audio::AudioDecoder>(TRANSFER_BUFFER_SIZE, TRANSFER_BUFFER_SIZE);

    esp_err_t err = decoder->start(this_pipeline->current_audio_file_type_);
    decoder->add_source(this_pipeline->raw_file_ring_buffer_);

    if (err != ESP_OK) {
      // Send specific error message
      event.err = err;
      xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);

      // Setting up the decoder failed, stop the pipeline
      xEventGroupSetBits(this_pipeline->event_group_,
                         EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
    }

    bool has_stream_info = false;

    while (true) {
      event_bits = xEventGroupGetBits(this_pipeline->event_group_);

      if (event_bits & PIPELINE_COMMAND_STOP) {
        break;
      }

      // Stop gracefully if the reader has finished
      audio::AudioDecoderState decoder_state = decoder->decode(event_bits & READER_MESSAGE_FINISHED);

      if ((decoder_state == audio::AudioDecoderState::DECODING) ||
          (decoder_state == audio::AudioDecoderState::FINISHED)) {
        this_pipeline->playback_ms_ += decoder->compute_play_duration_differential_ms();
      }

      if (decoder_state == audio::AudioDecoderState::FINISHED) {
        break;
      } else if (decoder_state == audio::AudioDecoderState::FAILED) {
        if (!has_stream_info) {
          event.decoding_err = DecodingError::FAILED_HEADER;
          xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);
        }
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        break;
      }

      if (!has_stream_info && decoder->get_audio_stream_info().has_value()) {
        has_stream_info = true;

        this_pipeline->current_audio_stream_info_ = decoder->get_audio_stream_info().value();

        // Send the stream information to the pipeline
        event.audio_stream_info = this_pipeline->current_audio_stream_info_;

        if (this_pipeline->current_audio_stream_info_.bits_per_sample != 16) {
          // Error state, incompatible bits per sample
          event.decoding_err = DecodingError::INCOMPATIBLE_BITS_PER_SAMPLE;
          xEventGroupSetBits(this_pipeline->event_group_,
                             EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        } else if ((this_pipeline->current_audio_stream_info_.channels > 2)) {
          // Error state, incompatible number of channels
          event.decoding_err = DecodingError::INCOMPATIBLE_CHANNELS;
          xEventGroupSetBits(this_pipeline->event_group_,
                             EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        } else {
#if !defined(SIMPLE_MEDIA_PLAYER)
          if ((this_pipeline->current_audio_stream_info_.channels < 2) ||
              (this_pipeline->current_audio_stream_info_.sample_rate != this_pipeline->target_sample_rate_)) {
            // Audio format requires resampling, allocate the decoded ring buffer and inform the resampler that the
            // stream information is available

            std::shared_ptr<RingBuffer> temp_ring_buffer;

            if (!this_pipeline->decoded_ring_buffer_.use_count()) {
              temp_ring_buffer = std::move(RingBuffer::create(
                  DECODED_BUFFER_DURATION_MS * this_pipeline->current_audio_stream_info_.sample_rate *
                  this_pipeline->current_audio_stream_info_.channels * sizeof(int16_t) / 1000));
              this_pipeline->decoded_ring_buffer_ = temp_ring_buffer;
            }

            if (!this_pipeline->decoded_ring_buffer_.use_count()) {
              // Allocating the ring buffer failed, stop the pipeline
              event.err = ESP_ERR_NO_MEM;

              xEventGroupSetBits(this_pipeline->event_group_,
                                 EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
            } else {
              decoder->add_sink(this_pipeline->decoded_ring_buffer_);
              xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_LOADED_STREAM_INFO);
            }
          } else {
            // Audio format doesn't require resampling, send it directly to the output
            if (this_pipeline->speaker_ != nullptr) {
              decoder->add_sink(this_pipeline->speaker_);
            }
          }
#else
          if (this_pipeline->speaker_ != nullptr) {
            this_pipeline->speaker_->set_audio_stream_info(this_pipeline->current_audio_stream_info_);
            decoder->add_sink(this_pipeline->speaker_);
          }
#endif
        }
        xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);
      }
    }
  }

  xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_FINISHED);
  vTaskDelete(NULL);
}

#if !defined(SIMPLE_MEDIA_PLAYER)
void AudioPipeline::resample_task(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  EventBits_t event_bits =
      xEventGroupWaitBits(this_pipeline->event_group_,
                          DECODER_MESSAGE_LOADED_STREAM_INFO | PIPELINE_COMMAND_STOP,  // Bit message to read
                          pdFALSE,                                                     // Clear the bit on exit
                          pdFALSE,                                                     // Wait for all the bits,
                          portMAX_DELAY);  // Block indefinitely until bit is set

  if (!(event_bits & PIPELINE_COMMAND_STOP)) {
    xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::RESAMPLER_MESSAGE_FINISHED |
                                                          EventGroupBits::DECODER_MESSAGE_LOADED_STREAM_INFO);
    InfoErrorEvent event;
    event.source = InfoErrorSource::RESAMPLER;

    std::unique_ptr<audio::AudioResampler> resampler =
        make_unique<audio::AudioResampler>(TRANSFER_BUFFER_SIZE, TRANSFER_BUFFER_SIZE);

    esp_err_t err = resampler->start(this_pipeline->current_audio_stream_info_, this_pipeline->target_sample_rate_,
                                     this_pipeline->current_resample_info_);
    resampler->add_source(this_pipeline->decoded_ring_buffer_);
    resampler->add_sink(this_pipeline->speaker_);

    if (err != ESP_OK) {
      // Send specific error message
      event.err = err;
      xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);

      // Setting up the resampler failed, stop the pipeline
      xEventGroupSetBits(this_pipeline->event_group_,
                         EventGroupBits::RESAMPLER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
    } else {
      event.resample_info = this_pipeline->current_resample_info_;
      xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);
    }

    while (true) {
      event_bits = xEventGroupGetBits(this_pipeline->event_group_);

      if (event_bits & PIPELINE_COMMAND_STOP) {
        break;
      }

      // Stop gracefully if the decoder is done
      audio::AudioResamplerState resampler_state = resampler->resample(event_bits & DECODER_MESSAGE_FINISHED);

      if (resampler_state == audio::AudioResamplerState::FINISHED) {
        break;
      } else if (resampler_state == audio::AudioResamplerState::FAILED) {
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::RESAMPLER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        break;
      }
    }
  }

  xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::RESAMPLER_MESSAGE_FINISHED);
  vTaskDelete(NULL);
}
#endif

}  // namespace speaker
}  // namespace esphome

#endif
