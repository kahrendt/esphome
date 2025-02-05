#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "esphome/components/audio/audio.h"
#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

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
  static void decode_task(void *params);
  TaskHandle_t snapcast_task_handle_{nullptr};
  TaskHandle_t decode_task_handle_{nullptr};
  uint16_t time_sync_counter_{0};

  audio::AudioFileType current_audio_file_type_{audio::AudioFileType::FLAC};
  optional<audio::AudioStreamInfo> current_audio_stream_info_;

  std::weak_ptr<RingBuffer> raw_file_ring_buffer_;

  static void time_sync_callback(void *params);

  // int64_t time_offsets_[50];
  std::vector<int64_t> time_offsets_;
  uint8_t time_offsets_index_{0};
  // uint8_t time_offsets_in_set_{0};

  int64_t previous_median_offset_{0};
  int64_t accumulated_drift_{0};

  bool decoder_pause_{false};

  int64_t update_time_offsets_(int64_t new_offset) {
    if (this->time_offsets_.size() < 50) {
      this->time_offsets_.push_back(new_offset);
    } else {
      this->time_offsets_[this->time_offsets_index_] = new_offset;
    }
    printf("time offest index %d\n", this->time_offsets_index_);
    ++this->time_offsets_index_;
    if (this->time_offsets_index_ == 50) {
      this->time_offsets_index_ = 0;
    }
    // this->time_offsets_index_ = std::min(this->time_offsets_index_ + 1, (int) (50));

    std::vector<int64_t> sorted_offsets;
    for (const int64_t &offset : this->time_offsets_) {
      sorted_offsets.push_back(offset);
    }
    std::sort(sorted_offsets.begin(), sorted_offsets.end());

    int64_t median_offset = sorted_offsets[sorted_offsets.size() / 2];
    if (sorted_offsets.size() % 2 == 0) {
      median_offset = (sorted_offsets[sorted_offsets.size() / 2] + sorted_offsets[sorted_offsets.size() / 2 + 1]) / 2;
    }

    int64_t drift_since_last_offset = 0;
    if (this->time_offsets_.size() > 1) {
      drift_since_last_offset = median_offset - this->previous_median_offset_;
    }
    this->previous_median_offset_ = median_offset;
    this->accumulated_drift_ += drift_since_last_offset;
    return this->accumulated_drift_;
  }
};

}  // namespace snapcast
}  // namespace esphome
#endif
