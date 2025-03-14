#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "median_filter.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include <deque>

namespace esphome {
namespace snapcast {

enum MessageType {
  SNAPCAST_MESSAGE_BASE = 0,
  SNAPCAST_MESSAGE_CODEC_HEADER = 1,
  SNAPCAST_MESSAGE_WIRE_CHUNK = 2,
  SNAPCAST_MESSAGE_SERVER_SETTINGS = 3,
  SNAPCAST_MESSAGE_TIME = 4,
  SNAPCAST_MESSAGE_HELLO = 5,
  SNAPCAST_MESSAGE_CLIENT_INFO = 7,
};

enum class SnapcastCodecFormat {
  SNAPCAST_CODEC_FLAC,
  SNAPCAST_CODEC_OPUS,
  SNAPCAST_CODEC_PCM,
  SNAPCAST_CODEC_UNSUPPORTED,
};

struct PlaybackProgress {
  uint32_t frames_played;
  int64_t write_timestamp;
};

// Stores encoded audio chunks sent from the server
struct AudioChunk {
  uint8_t *data;             // Pointer to encoded audio data. Must be deallocated after receiving
  size_t offset;             // Number of bytes to skip in the data pointer to skip
  size_t size;               // Number of bytes to read from the data pointer after the offset
  int64_t server_timestamp;  // Server timestamp when this part of the stream was recorded
  bool codec_header;         // True of this chunk contains only the codec header, not audio data
};

// Stores the timing information for decoded chunks of audio sent to the ESPHome speaker
struct InternalAudioTiming {
  int64_t server_timestamp;   // Server timestamp when this audio chunk should finish playing
  uint32_t total_frames;      // Total number of audio frames in this chunk, including corrections
  int32_t frame_corrections;  // Number of frames in this added/removed by the decoder to maintain sync
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
  const char *mac;
  const char *hostname;
  const char *version;
  const char *client_name;
  const char *os;
  const char *arch;
  int instance;
  const char *id;
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
static const size_t WIRE_CHUNK_HEADER_SIZE = 12;

class SnapcastPlayer : public Component, public media_player::MediaPlayer {
 public:
  SnapcastPlayer() : network_latency_filter_(MedianFilter(50)), actual_offsets_(MedianFilter(1)){};
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_CONNECTION; }
  // float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->is_muted_; }

  void start();

  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_server_address(std::string server_address) { this->server_address_ = std::move(server_address); }
  void set_server_port(uint16_t server_port) { this->server_port_ = server_port; }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  void control_get_server_status();

  Trigger<bool, float> *get_server_settings_trigger() const { return this->server_settings_trigger_; }

  void publish_client_settings() { this->send_client_message_(); }

 protected:
  // Receives commands from HA or from the voice assistant component
  void control(const media_player::MediaPlayerCall &call) override;

  void control_snapcast_stream_(media_player::MediaPlayerCommand command);

  void parse_snapcast_server_(JsonObject server);
  void parse_snapcast_groups_(JsonArray groups);
  void parse_snapcast_streams_(JsonArray streams);

  int64_t server_timestamp_to_client_(int64_t server_timestamp);
  ssize_t read_from_socket_(socket::Socket *socket, uint8_t *buffer, size_t length);
  std::string read_until_newline_(socket::Socket *socket);
  void control_set_stream_volume_(int volume);

  void base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);
  void base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);

  std::string hello_message_serialize_();

  std::string build_hello_message_(HelloMessage *msg);

  bool server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str);

  std::string client_message_serialize_(ClientInfoMessage *msg);

  esp_err_t connect_to_server_();
  void disconnect_from_server_();

  esp_err_t send_client_message_();
  esp_err_t send_hello_message_();
  esp_err_t send_time_message_();

  void clear_chunk_queue_();

  static void client_task(void *params);
  TaskHandle_t client_task_handle_{nullptr};
  StaticTask_t client_task_stack_;
  StackType_t *client_task_stack_buffer_{nullptr};

  static void control_task(void *params);
  TaskHandle_t control_task_handle_{nullptr};
  StaticTask_t control_task_stack_;
  StackType_t *control_task_stack_buffer_{nullptr};

  static void decode_task(void *params);
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};

  static void timesync_callback(void *params);
  uint16_t time_sync_counter_{0};

  EventGroupHandle_t event_group_{nullptr};

  optional<audio::AudioStreamInfo> audio_stream_info_;
  optional<SnapcastCodecFormat> codec_format_{SnapcastCodecFormat::SNAPCAST_CODEC_UNSUPPORTED};

  optional<uint16_t> volume_;

  bool task_stack_in_psram_{false};

  bool connected_{false};
  bool is_muted_{false};
  bool external_mute_{false};

  std::string group_id_{""};
  std::string player_id_{""};
  std::string stream_id_{""};
  optional<bool> stream_is_idle_;

  std::string album_{""};
  std::string artist_{""};
  std::string track_{""};

  speaker::Speaker *speaker_{nullptr};

  std::unique_ptr<socket::Socket> client_socket_;
  std::unique_ptr<socket::Socket> control_socket_;

  optional<std::string> server_address_;
  uint16_t server_port_;
  uint16_t server_control_port_{1705};

  QueueHandle_t playback_progress_queue_;

  QueueHandle_t encoded_chunk_data_queue_;

  size_t snapcast_buffer_duration_ms_{0};
  uint32_t snapcast_latency_ms_{0};

  MedianFilter network_latency_filter_;
  MedianFilter actual_offsets_;

  Trigger<bool, float> *server_settings_trigger_ = new Trigger<bool, float>();
};

template<typename... Ts> class PublishClientSettingsAction : public Action<Ts...>, public Parented<SnapcastPlayer> {
  void play(Ts... x) override { this->parent_->publish_client_settings(); }
};

}  // namespace snapcast
}  // namespace esphome
#endif
