#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "esphome/components/audio/audio.h"
#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include <deque>
namespace esphome {
namespace snapcast {

static const size_t MAX_CHUNK_SIZE = 8000;

enum MessageType {
  SNAPCAST_MESSAGE_BASE = 0,
  SNAPCAST_MESSAGE_CODEC_HEADER = 1,
  SNAPCAST_MESSAGE_WIRE_CHUNK = 2,
  SNAPCAST_MESSAGE_SERVER_SETTINGS = 3,
  SNAPCAST_MESSAGE_TIME = 4,
  SNAPCAST_MESSAGE_HELLO = 5,
  SNAPCAST_MESSAGE_CLIENT_INFO = 7,
};

struct PlaybackInfo {
  uint32_t frames_played;
  int64_t write_timestamp;
};

struct AudioSyncChunk {
  int64_t server_timestamp;
  size_t size;
  bool codec_header = false;
  uint8_t data[MAX_CHUNK_SIZE];
  size_t offset{0};
};

struct AudioSyncChunkTimings {
  int64_t server_timestamp;
  uint32_t total_frames;
  int32_t frame_corrections = 0;
  int64_t internal_timestamp;
};

struct TimeMessage {
  int32_t sec;
  int32_t usec;
};

struct BaseMessage {
  uint16_t type;
  uint16_t id;
  uint16_t refers_to;
  TimeMessage sent;
  TimeMessage received;
  uint32_t size;
};

struct HelloMessage {
  char *mac;
  const char *hostname;
  const char *version;
  const char *client_name;
  const char *os;
  const char *arch;
  int instance;
  char *id;
  int protocol_version;
};

struct ServerSettingsMessage {
  int32_t buffer_ms;
  int32_t latency;
  uint32_t volume;
  bool muted;
};

struct ClientInfoMessage {
  uint32_t volume;
  bool muted;
};

static const size_t BASE_MESSAGE_SIZE = 26;
static const size_t TIME_MESSAGE_SIZE = 8;

class MedianFilter {
 public:
  MedianFilter(uint8_t capacity) : capacity_(capacity) { this->data_.reserve(capacity); };

  int64_t update(int64_t new_value) {
    if (this->data_.size() < this->capacity_) {
      this->data_.push_back(new_value);
    } else {
      this->data_[this->index_] = new_value;
    }
    ++this->index_;
    if (this->index_ == this->capacity_) {
      this->index_ = 0;
    }

    std::vector<int64_t> sorted_data;
    for (const int64_t &value : this->data_) {
      sorted_data.push_back(value);
    }
    std::sort(sorted_data.begin(), sorted_data.end());

    int64_t median_offset = sorted_data[sorted_data.size() / 2];
    if (sorted_data.size() % 2 == 0) {
      median_offset = sorted_data[sorted_data.size() / 2] / 2 + sorted_data[sorted_data.size() / 2 + 1] / 2;
    }

    this->most_recent_median_ = median_offset;
    return median_offset;
  }

  int64_t get_most_recent_median() { return this->most_recent_median_; }

  bool is_full() { return (this->data_.size() == this->capacity_); }

  void reset() {
    this->data_.clear();
    this->most_recent_median_ = 0;
  }

 protected:
  int64_t most_recent_median_{0};
  std::vector<int64_t> data_;
  uint8_t index_{0};
  uint8_t capacity_;
};

class SnapcastPlayer : public Component {
 public:
  SnapcastPlayer()
      : internal_latency_(MedianFilter(50)),
        server_internal_clock_offset_(MedianFilter(50)),
        actual_offsets_(MedianFilter(1)){};
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  void setup() override { this->start(); }
  void loop() override;

  void start();

  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_server_address(std::string server_address) { this->server_address_ = std::move(server_address); }
  void set_server_port(uint16_t server_port) { this->server_port_ = server_port; }

  esp_err_t send_client_message();

 protected:
  speaker::Speaker *speaker_{nullptr};
  std::unique_ptr<socket::Socket> socket_;

  optional<std::string> server_address_;
  uint16_t server_port_;

  QueueHandle_t playback_info_queue_;

  int64_t server_timestamp_to_client_(int64_t server_timestamp);

  void base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);
  void base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);

  std::string hello_message_serialize_();

  std::string build_hello_message_(HelloMessage *msg);

  bool server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str);

  std::string client_message_serialize_(ClientInfoMessage *msg);
  esp_err_t connect_to_server_();

  esp_err_t send_hello_message_();
  static void timesync_callback(void *params);
  static void snapcast_task(void *params);
  static void decode_task(void *params);
  static void sync_task(void *params);
  TaskHandle_t snapcast_task_handle_{nullptr};
  TaskHandle_t decode_task_handle_{nullptr};
  TaskHandle_t sync_task_handle_{nullptr};
  uint16_t time_sync_counter_{0};

  EventGroupHandle_t event_group_{nullptr};

  optional<audio::AudioStreamInfo> current_audio_stream_info_;

  esp_err_t send_time_message_();

  optional<uint16_t> volume_;

  bool connected_{false};

  bool first_audio_played_{true};
  int64_t initial_playback_timestamp_{0};

  QueueHandle_t encoded_chunk_data_queue_;
  StaticQueue_t encoded_chunk_data_queue_buffer_;
  uint8_t *encoded_chunk_data_queue_storage_{nullptr};

  QueueHandle_t decoded_chunk_data_queue_;
  StaticQueue_t decoded_chunk_data_queue_buffer_;
  uint8_t *decoded_chunk_data_queue_storage_{nullptr};

  std::deque<AudioSyncChunkTimings> chunk_timings_;

  size_t snapcast_buffer_duration_ms_{0};
  uint32_t snapcast_latency_ms_{0};

  int32_t pending_frame_corrections_{0};

  bool external_mute_{false};

  MedianFilter internal_latency_;
  MedianFilter server_internal_clock_offset_;
  MedianFilter actual_offsets_;
};

}  // namespace snapcast
}  // namespace esphome
#endif
