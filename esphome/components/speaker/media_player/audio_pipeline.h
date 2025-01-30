#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_reader.h"
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/ring_buffer.h"

#include "esp_err.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace speaker {

// Internal sink/source buffers for reader and decoder
static const size_t DEFAULT_TRANSFER_BUFFER_SIZE = 24 * 1024;

enum class AudioPipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

enum class AudioPipelineState : uint8_t {
  PLAYING,
  STOPPED,
  PAUSED,
  ERROR_READING,
  ERROR_DECODING,
};

enum class InfoErrorSource : uint8_t {
  READER = 0,
  DECODER,
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
  optional<DecodingError> decoding_err;
};

class AudioPipeline {
 public:
  AudioPipeline(speaker::Speaker *speaker, size_t buffer_size, bool task_stack_in_psram)
      : task_stack_in_psram_(task_stack_in_psram), speaker_(speaker), buffer_size_(buffer_size) {
    this->allocate_buffers_();
    this->transfer_buffer_size_ = std::min(buffer_size_ / 4, DEFAULT_TRANSFER_BUFFER_SIZE);
  };

  /// @brief Starts an audio pipeline given a media url
  /// @param uri media file url
  /// @param task_name FreeRTOS task names
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t start_url(const std::string &uri, const std::string &task_name, UBaseType_t priority = 1);

  /// @brief Starts an audio pipeline given a AudioFile pointer
  /// @param audio_file pointer to a AudioFile object
  /// @param task_name FreeRTOS task name
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t start_file(audio::AudioFile *audio_file, const std::string &task_name, UBaseType_t priority = 1);

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
  /// @param task_name FreeRTOS task name
  /// @param priority FreeRTOS task priority
  /// @return ESP_OK if successful or an appropriate error if not
  esp_err_t common_start_(const std::string &task_name, UBaseType_t priority);

  /// @brief Resets the task related pointers and deallocates their stack.
  void delete_tasks_();

  uint32_t playback_ms_{0};

  bool start_in_progress_{false};
  bool pause_state_{false};
  bool task_stack_in_psram_;

  speaker::Speaker *speaker_{nullptr};

  std::string current_uri_{};
  audio::AudioFile *current_audio_file_{nullptr};

  audio::AudioFileType current_audio_file_type_;
  audio::AudioStreamInfo current_audio_stream_info_;

  size_t buffer_size_;           // Ring buffer between reader and decoder
  size_t transfer_buffer_size_;  // Internal source/sink buffers for the audio reader and decoder

  std::weak_ptr<RingBuffer> raw_file_ring_buffer_;

  // Handles basic control/state of the three tasks
  EventGroupHandle_t event_group_{nullptr};

  // Receives detailed info (file type, stream info, resampling info) or specific errors from the three tasks
  QueueHandle_t info_error_queue_{nullptr};

  // Handles reading the media file from flash or a url
  static void read_task(void *params);
  TaskHandle_t read_task_handle_{nullptr};
  StaticTask_t read_task_stack_;
  StackType_t *read_task_stack_buffer_{nullptr};

  // Decodes the media file into PCM audio
  static void decode_task(void *params);
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};
};

}  // namespace speaker
}  // namespace esphome

#endif
