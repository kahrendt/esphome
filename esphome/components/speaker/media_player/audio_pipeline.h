#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_files.h"
#include "esphome/components/audio/audio_reader.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/audio/audio_resampler.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esp_err.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace speaker {

// Internal sink/source buffers for reader, decoder, and resampler
static const size_t DEFAULT_TRANSFER_BUFFER_SIZE = 24 * 1024;

enum class AudioPipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

enum class AudioPipelineState : uint8_t {
  PLAYING,
  STOPPED,
  ERROR_READING,
  ERROR_DECODING,
#ifdef USE_SPEAKER_MEDIA_PLAYER_RESAMPLER
  ERROR_RESAMPLING,
#endif
};

enum class InfoErrorSource : uint8_t {
  READER = 0,
  DECODER,
#ifdef USE_SPEAKER_MEDIA_PLAYER_RESAMPLER
  RESAMPLER,
#endif
};

enum class DecodingError : uint8_t {
  FAILED_HEADER = 0,
  INCOMPATIBLE_BITS_PER_SAMPLE,
  INCOMPATIBLE_CHANNELS,
};

// Used to pass information from each task.
struct InfoErrorEvent {
  InfoErrorSource source;
  optional<esp_err_t> err;
  optional<audio::AudioFileType> file_type;
  optional<audio::AudioStreamInfo> audio_stream_info;
#ifdef USE_SPEAKER_MEDIA_PLAYER_RESAMPLER
  optional<audio::ResampleInfo> resample_info;
#endif
  optional<DecodingError> decoding_err;
};

class AudioPipeline {
 public:
  AudioPipeline(std::weak_ptr<RingBuffer> ring_buffer, size_t buffer_size)
      : output_ring_buffer_(ring_buffer), buffer_size_(buffer_size) {
    this->allocate_buffers_();
    this->transfer_buffer_size_ = std::min(buffer_size_ / 4, DEFAULT_TRANSFER_BUFFER_SIZE);
  };
  AudioPipeline(speaker::Speaker *speaker, size_t buffer_size) : speaker_(speaker), buffer_size_(buffer_size) {
    this->allocate_buffers_();
    this->transfer_buffer_size_ = std::min(buffer_size_ / 4, DEFAULT_TRANSFER_BUFFER_SIZE);
  };

  /// @brief Starts an audio pipeline given a media url
  /// @param uri media file url
  /// @param target_sample_rate the desired sample rate of the audio stream
  /// @param task_name FreeRTOS task names
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t start(const std::string &uri, uint32_t target_sample_rate, const std::string &task_name,
                  UBaseType_t priority = 1);

  /// @brief Starts an audio pipeline given a AudioFile pointer
  /// @param audio_file pointer to a AudioFile object
  /// @param target_sample_rate the desired sample rate of the audio stream
  /// @param task_name FreeRTOS task name
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t start(audio::AudioFile *audio_file, uint32_t target_sample_rate, const std::string &task_name,
                  UBaseType_t priority = 1);

  /// @brief Stops the pipeline. Sends a stop signal to each task (if running) and clears the ring buffers.
  /// @return ESP_OK if successful or ESP_ERR_TIMEOUT if the tasks did not indicate they stopped
  esp_err_t stop();

  /// @brief Gets the state of the audio pipeline based on the info_error_queue_ and event_group_
  /// @return AudioPipelineState
  AudioPipelineState get_state();

  /// @brief Suspends any running tasks
  void suspend_tasks();
  /// @brief Resumes any running tasks
  void resume_tasks();

  uint32_t get_playback_ms() { return this->playback_ms_; }

  void set_pause_state(bool pause_state);

 protected:
  /// @brief Allocates the ring buffers, event group, and info error queue.
  /// @return ESP_OK if successful or ESP_ERR_NO_MEM if it is unable to allocate all parts
  esp_err_t allocate_buffers_();

  /// @brief Common start code for the pipeline, regardless if the source is a file or url.
  /// @param target_sample_rate the desired sample rate of the audio stream
  /// @param task_name FreeRTOS task name
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t common_start_(uint32_t target_sample_rate, const std::string &task_name, UBaseType_t priority);

  uint32_t playback_ms_;

  // Pointer to the media player's mixer object. The resample task feeds the appropriate ring buffer directly
  // AudioMixer *mixer_;
  std::weak_ptr<RingBuffer> output_ring_buffer_;
  speaker::Speaker *speaker_;

  std::string current_uri_{};
  audio::AudioFile *current_audio_file_{nullptr};

  audio::AudioFileType current_audio_file_type_;
  audio::AudioStreamInfo current_audio_stream_info_;
  audio::ResampleInfo current_resample_info_;

  size_t buffer_size_;           // Ring buffer between reader and decoder
  size_t transfer_buffer_size_;  // Internal source/sink buffers for the audio reader, decoder, and resampler

  uint32_t target_sample_rate_;

  std::weak_ptr<RingBuffer> raw_file_ring_buffer_;
  std::weak_ptr<RingBuffer> decoded_ring_buffer_;

  // Handles basic control/state of the three tasks
  EventGroupHandle_t event_group_{nullptr};

  // Receives detailed info (file type, stream info, resampling info) or specific errors from the three tasks
  QueueHandle_t info_error_queue_{nullptr};

  // Handles reading the media file from flash or a url
  static void read_task(void *params);
  TaskHandle_t read_task_handle_{nullptr};

  // Decodes the media file into PCM audio
  static void decode_task(void *params);
  TaskHandle_t decode_task_handle_{nullptr};

#ifdef USE_SPEAKER_MEDIA_PLAYER_RESAMPLER
  // Resamples the audio to match the specified target sample rate. Converts mono audio to stereo audio if necessary.
  static void resample_task(void *params);
  TaskHandle_t resample_task_handle_{nullptr};
#endif
};

}  // namespace speaker
}  // namespace esphome

#endif
