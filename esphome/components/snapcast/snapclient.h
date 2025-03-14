#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "median_filter.h"

#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/socket/socket.h"

#include <freertos/queue.h>

#include "esp_check.h"

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

enum class ProcessMessageResponse {
  PROCESSED_CODEC_HEADER,
  PROCESSED_WIRE_CHUNK,
  PROCESSED_SERVER_SETTINGS,
  PROCESSED_TIME,
  ERROR_SOCKET_READ,
  ERROR_MISMATCHED_SIZES,
  ERROR_UNSUPPORTED_FORMAT,
  ERROR_DISCONNECTED,
  ERROR_FAILURE,
};

// Stores encoded audio chunks sent from the server
struct AudioChunk {
  uint8_t *data;             // Pointer to encoded audio data. Must be deallocated after receiving
  size_t offset;             // Number of bytes to skip in the data pointer to skip
  size_t size;               // Number of bytes to read from the data pointer after the offset
  int64_t server_timestamp;  // Server timestamp when this part of the stream was recorded
  bool codec_header;         // True of this chunk contains only the codec header, not audio data
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

class Snapclient {
 public:
  Snapclient(std::string player_id) : player_id_(player_id), network_latency_filter_(MedianFilter(50)){};

  esp_err_t connect_to_server(std::string server_address = "", uint16_t port = 1704);
  void disconnect_from_server();

  esp_err_t send_client_message(float volume, bool muted);
  esp_err_t send_hello_message();
  esp_err_t send_time_message();

  ServerSettingsMessage get_server_settings_message() { return this->server_settings_message_; }
  bool get_network_latency_full() { return this->network_latency_filter_.is_full(); }
  SnapcastCodecFormat get_codec_format() { return this->codec_format_; }

  esp_err_t read_base_message(BaseMessage *base_msg);
  ProcessMessageResponse process_messages(BaseMessage &base_msg, AudioChunk *audio_chunk);

  int64_t server_timestamp_to_client(int64_t server_timestamp);

 protected:
  void base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);
  void base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);

  std::string client_message_serialize_(ClientInfoMessage *msg);
  std::string hello_message_serialize_();

  std::string build_hello_message_(HelloMessage *msg);

  bool server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str);

  ssize_t read_from_socket_(socket::Socket *socket, uint8_t *buffer, size_t length);

  std::unique_ptr<socket::Socket> client_socket_;
  MedianFilter network_latency_filter_;

  ServerSettingsMessage server_settings_message_;

  uint16_t time_sync_counter_{0};
  bool is_connected_{false};
  std::string player_id_;
  SnapcastCodecFormat codec_format_{SnapcastCodecFormat::SNAPCAST_CODEC_UNSUPPORTED};
};
}  // namespace snapcast
}  // namespace esphome
#endif
