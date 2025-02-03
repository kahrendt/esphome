#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"

namespace esphome {
namespace snapcast {

enum message_type {
  SNAPCAST_MESSAGE_BASE = 0,
  SNAPCAST_MESSAGE_CODEC_HEADER = 1,
  SNAPCAST_MESSAGE_WIRE_CHUNK = 2,
  SNAPCAST_MESSAGE_SERVER_SETTINGS = 3,
  SNAPCAST_MESSAGE_TIME = 4,
  SNAPCAST_MESSAGE_HELLO = 5,
  SNAPCAST_MESSAGE_STREAM_TAGS = 6,

  SNAPCAST_MESSAGE_FIRST = SNAPCAST_MESSAGE_BASE,
  SNAPCAST_MESSAGE_LAST = SNAPCAST_MESSAGE_STREAM_TAGS
};

typedef struct tv {
  int32_t sec;
  int32_t usec;
} tv_t;

typedef struct base_message {
  uint16_t type;
  uint16_t id;
  uint16_t refersTo;
  tv_t sent;
  tv_t received;
  uint32_t size;
} base_message_t;

/* Sample Hello message
{
    "Arch": "x86_64",
    "ClientName": "Snapclient",
    "HostName": "my_hostname",
    "ID": "00:11:22:33:44:55",
    "Instance": 1,
    "MAC": "00:11:22:33:44:55",
    "OS": "Arch Linux",
    "SnapStreamProtocolVersion": 2,
    "Version": "0.17.1"
}
*/

typedef struct hello_message {
  char *mac;
  char *hostname;
  char *version;
  char *client_name;
  char *os;
  char *arch;
  int instance;
  char *id;
  int protocol_version;
} hello_message_t;

typedef struct server_settings_message {
  int32_t buffer_ms;
  int32_t latency;
  uint32_t volume;
  bool muted;
} server_settings_message_t;

// typedef struct codec_header_message {
//   char *codec;
//   uint32_t size;
//   char *payload;
// } codec_header_message_t;

// typedef struct wire_chunk_message {
//   tv_t timestamp;
//   size_t size;
//   char *payload;
// } wire_chunk_message_t;

// typedef struct time_message {
//   tv_t latency;
// } time_message_t;

static const size_t BASE_MESSAGE_SIZE = 26;
static const size_t TIME_MESSAGE_SIZE = 8;

class SnapcastPlayer : public Component {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;

  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }

 protected:
  speaker::Speaker *speaker_{nullptr};
  std::unique_ptr<socket::Socket> socket_;

  void base_message_serialize_(base_message_t *msg, bytebuffer::ByteBuffer &buffer);
  void base_message_deserialize_(base_message_t *msg, bytebuffer::ByteBuffer &buffer);

  std::string hello_message_serialize_();

  std::string build_hello_message_(hello_message_t *msg);

  bool server_settings_message_deserialize_(server_settings_message_t *msg, const char *json_str);

  // void codec_header_deserialize(codec_message_t *msg, bytebuffer::ByteBuffer &buffer);

  static void snapcast_task(void *params);
  TaskHandle_t snapcast_task_handle_{nullptr};
  // void close_connection_(struct netconn *conn);

  // bool codec_header_message_deserialize_(codec_header_message_t *msg, const char *data, uint32_t size);
  // void codec_header_message_free_(codec_header_message_t *msg);

  // // TODO currently copies, could be made to not copy probably
  // int wire_chunk_message_deserialize_(wire_chunk_message_t *msg, const char *data, uint32_t size);
  // void wire_chunk_message_free_(wire_chunk_message_t *msg);
};

}  // namespace snapcast
}  // namespace esphome
#endif
