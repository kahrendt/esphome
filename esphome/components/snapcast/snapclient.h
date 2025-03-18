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
  bool codec_header;         // True if this chunk contains only the codec header, not audio data
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
  /* Class that implements the binary protocl communication with a snapserver as snapclient.
   *
   * Based on https://github.com/badaix/snapcast/blob/develop/doc/binary_protocol.md (accessed 2025-03-18)
   *
   * 1) First connect to the snapserver with ``connect_to_server``
   * 2) Send a hello message with ``send_hello_message``
   * 3) Regularly send time messages with ``send_time_message``
   *   - a ``MedianFilter`` object uses a median over the last 50 time messages for computing the server client latency
   *     - Use the ``get_network_latency_full`` function to determine if enough time messages have been sent to compute
   *       an accurate latency
   *   - Convert a server timestamp, in microseconds, to the client microsecond timestamp using
   *     ``server_timestamp_to_client``
   * 4) After a connection is established, regular call ``read_base_message`` and ``process_messages``
   *   - The caller is responsible for allocating (and deallocating) an AudioChunk to store incoming data in
   *     ``process_messages``
   *
   * Use ``send_client_message`` to inform the snapserver about the clients mute and volume states
   */
 public:
  Snapclient(std::string player_id) : player_id_(player_id), network_latency_filter_(MedianFilter(50)){};

  /// @brief Connects to a snapserver's client tcp interface.
  /// @param server_address (std::string) Server IP address
  /// @param port (uint16_t) Port for the snapserver's client tcp interface
  /// @return ESP_OK if successful, ESP_FAIL if there was a problem connecting the socket
  esp_err_t connect_to_server(std::string server_address, uint16_t port);

  /// @brief Disconnects and shuts down the socket.
  void disconnect_from_server();

  /// @brief Sends a snapcast client message to the server to update the current volume and muted states.
  /// @param volume (float) current volume
  /// @param muted (bool) current muted state
  /// @return ESP_FAIL if the message couldn't be sent, ESP_OK otherwise
  esp_err_t send_client_message(float volume, bool muted);

  /// @brief Sends an inital snapcast hello message to the server describing the client.
  /// @return ESP_FAIL if the message couldn't be sent, ESP_OK otherwise
  esp_err_t send_hello_message();

  /// @brief Sends a snapcast time message to the server with clients current internal time.
  /// @return ESP_FAIL if the message couldn't be sent, ESP_OK otherwise
  esp_err_t send_time_message();

  /// @brief Gets the most recently sent server settings.
  /// @return ServerSettingsMessage most recent server settings
  ServerSettingsMessage get_server_settings() { return this->server_settings_message_; }

  /// @brief Tests if the median filter for the server client timestamp offset is full.
  /// @return True if full, false otherwise.
  bool is_network_latency_full() { return this->network_latency_filter_.is_full(); }

  /// @brief Gets the current stream's codec.
  /// @return (SnapcastCodecFormat)
  SnapcastCodecFormat get_codec_format() { return this->codec_format_; }

  /// @brief Reads a base message from the socket.
  /// @param base_msg (BaseMessage*) Stores the read, deserialized base message
  /// @return ESP_FAIL if the message couldn't be read, ESP_OK otherwise
  esp_err_t read_base_message(BaseMessage *base_msg);

  /// @brief Reads a follow-up message from the socket and processes its information.
  /// @param base_msg (BaseMessage&) The base message describing this follow-up message
  /// @param audio_chunk (AudioChunk*) Stores the incoming message. Must already have allocated the .data pointer.
  /// @return (ProcessMessageResponse) The type of the message processed if successful, or an appropriate error.
  ProcessMessageResponse process_messages(BaseMessage &base_msg, AudioChunk *audio_chunk);

  /// @brief Converts a server timestamp into a client timestamp taking into account the network latency.
  /// @param server_timestamp (int64_t) Server timestamp in microseconds
  /// @return (int64_t) Client timestamp in microseconds
  int64_t server_timestamp_to_client(int64_t server_timestamp);

  /// @brief Tests if the socket is currently connected.
  /// @return True if there is an active connection, false otherwise
  bool is_connected() { return this->is_connected_; }

 protected:
  /// @brief Serializes a base message into a bytebuffer for sending to the server.
  /// @param msg (BaseMessage *) Base message to serialize
  /// @param buffer (ByteBuffer&) Buffer to store the serialized message
  void base_message_serialize_(const BaseMessage *msg, bytebuffer::ByteBuffer &buffer);

  /// @brief Deserializes a base message in a bytebuffer received from the server.
  /// @param msg (BaseMessage *) Base message to store deserialized message
  /// @param buffer (ByteBuffer&) Buffer containing the message to deserialize
  void base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer);

  /// @brief Serializes a client message into a JSON string for sending to the server.
  /// @param msg (ClientInfoMessage *) Client message to serialize
  /// @return (std::string) Message serialized into JSON format
  std::string client_message_serialize_(const ClientInfoMessage *msg);

  /// @brief Builds a HelloMessage struct containing information about the client.
  /// @return (HelloMessage) The client's hello message
  HelloMessage build_hello_message_() const;

  /// @brief Serializes a client hello message into a JSON string.
  /// @param msg (HelloMessage *) Message to serialize
  /// @return (std::string) Hello message serialized into JSON format
  std::string hello_message_serialize_(const HelloMessage *msg);

  // TODO: why return a boolean?

  /// @brief Deserializes a server settings message JSON string.
  /// @param msg (ServerSettingsMessage*) Stores the deserialized string
  /// @param json_str (const char*) Server settings JSON formatted string to deserialize
  /// @return True if successful, false otherwse
  bool server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str);

  /// @brief Reads a specified number of bytes from the socket into a buffer.
  /// Repeatedly reads until the specified number of bytes are read.
  /// @param buffer (uint8_t*) Buffer to store received data
  /// @param length (size_t) Number of bytes to read
  /// @return (ssize_t) -1 if failed, otherwise the number of bytes received
  ssize_t read_from_socket_(uint8_t *buffer, size_t length);

  std::unique_ptr<socket::Socket> socket_;
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
