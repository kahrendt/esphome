#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/audio/audio_decoder.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mdns.h"

#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "esp_mac.h"

#include <esp_timer.h>

#include <flac_decoder.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

static const size_t INPUT_BUFFER_SIZE = 1024 * 50;
static const size_t OUTPUT_BUFFER_SIZE = 1024 * 50;

static const uint32_t ENCODED_CHUNK_QUEUE_SIZE = 50;
static const uint32_t DECODED_CHUNK_QUEUE_SIZE = 50;

static const uint32_t FAST_SYNC_LATENCY_BUF = 10000;      // in µs
static const uint32_t NORMAL_SYNC_LATENCY_BUF = 1000000;  // in µs

static const size_t CONTROL_TASK_STACK_SIZE = 3 * 1024;
static const size_t SNAPCAST_TASK_STACK_SIZE = 3 * 1024;
static const size_t DECODE_TASK_STACK_SIZE = 3 * 1024;
static const size_t SYNC_TASK_STACK_SIZE = 3 * 1024;

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),
  DECODE_FINISHED = (1 << 3),
  SYNC_FINISHED = (1 << 5),
  CONTROL_START = (1 << 7),
};

void SnapcastPlayer::start() {
  this->speaker_->add_audio_output_callback([this](uint32_t frames_played, int64_t write_timestamp) {
    PlaybackInfo playback_info = {.frames_played = frames_played, .write_timestamp = write_timestamp};
    if (!xQueueSend(this->playback_info_queue_, &playback_info, 0)) {
      ESP_LOGE(TAG, "Playback info queue was full");
    }
  });

  RAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->encoded_chunk_data_queue_storage_ = allocator.allocate(ENCODED_CHUNK_QUEUE_SIZE * sizeof(AudioSyncChunk));
  if (this->encoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->encoded_chunk_data_queue_ =
      xQueueCreateStatic(ENCODED_CHUNK_QUEUE_SIZE, sizeof(AudioSyncChunk), this->encoded_chunk_data_queue_storage_,
                         &encoded_chunk_data_queue_buffer_);

  this->decoded_chunk_data_queue_storage_ = allocator.allocate(DECODED_CHUNK_QUEUE_SIZE * sizeof(AudioSyncChunk));
  if (this->decoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }

  this->decoded_chunk_data_queue_ =
      xQueueCreateStatic(DECODED_CHUNK_QUEUE_SIZE, sizeof(AudioSyncChunk), this->decoded_chunk_data_queue_storage_,
                         &decoded_chunk_data_queue_buffer_);
  // this->encoded_chunk_data_queue_ = xQueueCreate(20, sizeof(AudioSyncChunk));
  xTaskCreate(snapcast_task, "snapcast", SNAPCAST_TASK_STACK_SIZE, (void *) this, 5, &this->snapcast_task_handle_);
  xTaskCreate(control_task, "snap_control", CONTROL_TASK_STACK_SIZE, (void *) this, 1, &this->control_task_handle_);

  this->playback_info_queue_ = xQueueCreate(10, sizeof(PlaybackInfo));
  this->event_group_ = xEventGroupCreate();
}

void SnapcastPlayer::loop() {
  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if (event_bits & (DECODE_FINISHED | SYNC_FINISHED)) {
    if (this->speaker_->is_stopped()) {
      xQueueReset(this->playback_info_queue_);
      xEventGroupClearBits(this->event_group_, (COMMAND_STOP | DECODE_FINISHED | SYNC_FINISHED));
    } else {
      this->speaker_->stop();
    }
  }
  if (this->volume_.has_value()) {
    this->volume = static_cast<float>(this->volume_.value()) / 100.0f;
    this->speaker_->set_volume(this->volume);
    this->send_client_message_();
    this->publish_state();
    this->volume_.reset();
  }

  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;

  if (this->state != old_state) {
    this->publish_state();
    ESP_LOGD(TAG, "State changed to %s", media_player::media_player_state_to_string(this->state));
  }
}

esp_err_t SnapcastPlayer::send_client_message_() {
  if (this->connected_) {
    ClientInfoMessage client_msg = {.volume = static_cast<uint32_t>(this->speaker_->get_volume() * 100.0f),
                                    .muted = this->external_mute_};
    std::string json_client_msg = this->client_message_serialize_(&client_msg);
    size_t client_message_size = json_client_msg.size() + sizeof(uint32_t);

    int64_t now = esp_timer_get_time();
    bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE + sizeof(uint32_t));
    BaseMessage base_msg_for_client_info = {
        .type = SNAPCAST_MESSAGE_CLIENT_INFO,
        .id = 0x0000,
        .refers_to = 0x0000,
        .sent = {.sec = static_cast<int32_t>(now / 1000000LL),
                 .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
        .received = {.sec = 0, .usec = 0},
        .size = client_message_size,
    };
    this->base_message_serialize_(&base_msg_for_client_info, base_msg_buffer);
    base_msg_buffer.put_uint32(json_client_msg.size());

    if ((this->client_socket_->write((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE + sizeof(uint32_t)) ==
         -1) ||
        (this->client_socket_->write((void *) json_client_msg.data(), json_client_msg.size()) == -1)) {
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t SnapcastPlayer::send_time_message_() {
  bytebuffer::ByteBuffer time_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  int64_t now = esp_timer_get_time();
  BaseMessage base_msg_for_time = {
      .type = SNAPCAST_MESSAGE_TIME,
      .id = this->time_sync_counter_++,
      .refers_to = 0x0000,
      .sent = {.sec = static_cast<int32_t>(now / 1000000LL),
               .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
      .received = {.sec = 0, .usec = 0},
      .size = TIME_MESSAGE_SIZE,
  };

  this->base_message_serialize_(&base_msg_for_time, time_msg_buffer);
  time_msg_buffer.put_int32(0);
  time_msg_buffer.put_int32(0);
  ssize_t time_msg_written =
      this->client_socket_->write((void *) time_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  if (time_msg_written == -1) {
    return ESP_FAIL;
  }
  if (time_msg_written < BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE) {
    ESP_LOGE(TAG, "Time message didn't fully send!");
  }
  return ESP_OK;
}

void SnapcastPlayer::timesync_callback(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  this_snapcast->send_time_message_();
}

esp_err_t SnapcastPlayer::send_hello_message_() {
  std::string hello_msg = this->hello_message_serialize_();

  size_t total_hello_msg_size = hello_msg.size() + sizeof(uint32_t);
  bytebuffer::ByteBuffer hello_msg_buffer = bytebuffer::ByteBuffer(total_hello_msg_size);

  hello_msg_buffer.put_uint32(total_hello_msg_size);
  for (size_t i = 0; i < hello_msg.size(); ++i) {
    hello_msg_buffer.put_uint8(hello_msg.data()[i]);
  }
  int64_t now = esp_timer_get_time();

  BaseMessage base_msg = {
      .type = SNAPCAST_MESSAGE_HELLO,
      .id = 0x0000,
      .refers_to = 0x0000,
      .sent = {.sec = static_cast<int32_t>(now / 1000000),
               .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
      .received = {.sec = 0, .usec = 0},
      .size = total_hello_msg_size,
  };

  bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

  this->base_message_serialize_(&base_msg, base_msg_buffer);

  if ((this->client_socket_->write((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE) == -1) ||
      (this->client_socket_->write((void *) hello_msg_buffer.get_raw_data(), base_msg.size) == -1)) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t SnapcastPlayer::connect_to_server_() {
  // Connect to configured server, if set. Otherwise, use mdns to discover a server

  uint16_t port = this->server_port_;
  esp_err_t err = ESP_OK;

  socklen_t sl = 0;
  socklen_t sl_control = 0;
  struct sockaddr_storage server;
  struct sockaddr_storage server_control;

  if (!this->server_address_.has_value()) {
    mdns_result_t *mdns_result;
    char ip_address[16];

    mdns_init();

    ESP_LOGI(TAG, "Lookup snapcast service on network");
    err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &mdns_result);

    if (!mdns_result) {
      ESP_LOGW(TAG, "No results found for snapcast service!");
      return ESP_FAIL;
    } else {
      if (mdns_result->addr) {
        sprintf(ip_address, "%d.%d.%d.%d", IP2STR(mdns_result->addr));
        port = mdns_result->port;
        ESP_LOGD(TAG, "Found a snapcast server via mdns: " IPSTR, IP2STR(mdns_result->addr));
      }
      mdns_query_results_free(mdns_result);
    }

    sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), (const char *) ip_address, port);
    sl_control = socket::set_sockaddr((struct sockaddr *) &server_control, sizeof(server_control),
                                      (const char *) ip_address, this->server_control_port_);
  } else {
    sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), this->server_address_.value().c_str(), port);
    sl_control = socket::set_sockaddr((struct sockaddr *) &server_control, sizeof(server_control),
                                      this->server_address_.value().c_str(), this->server_control_port_);
  }

  if ((sl == 0) || (sl_control == 0)) {
    ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
    return ESP_FAIL;
  }
  this->client_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);
  this->control_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);

  err = this->client_socket_->connect((struct sockaddr *) &server, sizeof(server));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to connect: errno %d", err);
    return ESP_FAIL;
  }
  err = this->control_socket_->connect((struct sockaddr *) &server_control, sizeof(server_control));
  if (err != 0) {
    ESP_LOGE(TAG, "Control socket unable to connect: errno %d", err);
    return ESP_FAIL;
  }
  xEventGroupSetBits(this->event_group_, CONTROL_START);
  // this->control_rpc_version_();
  // printf("received control rpc version\n");

  int nodelay = 1;
  if (this->client_socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
    ESP_LOGW(TAG, "Failed to turn on TCP_NODELAY, syncing may not be accurate");
    nodelay = 0;
  }

  return ESP_OK;
}

media_player::MediaPlayerTraits SnapcastPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();

  traits.set_supports_pause(true);

  return traits;
}

void SnapcastPlayer::setup() {
  char mac_address[18];
  uint8_t base_mac[6];
  esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
  sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4],
          base_mac[5]);

  this->player_id_ = std::string(mac_address);
  this->start();
}

void SnapcastPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!this->is_ready()) {
    // Ignore any commands sent before the media player is setup
    return;
  }

  if (call.get_volume().has_value()) {
    this->volume_ = round(call.get_volume().value() * 100.0f);
  }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      default:
        break;
    }
  }
}

ssize_t SnapcastPlayer::read_from_socket_(socket::Socket *socket, uint8_t *buffer, size_t length) {
  size_t offset = 0;
  while (length > 0) {
    ssize_t bytes_read = socket->read((void *) (buffer + offset), length);

    if (bytes_read == -1) {
      return -1;
    }
    length -= bytes_read;
    offset += bytes_read;
  }

  return offset;
}

std::string SnapcastPlayer::read_until_newline_(socket::Socket *socket) {
  std::string buffer;
  char new_char = ' ';
  while (new_char != '\n') {
    ssize_t bytes_read = socket->read((void *) &new_char, sizeof(char));
    if (bytes_read == -1) {
      printf("reading from control socket had an issue!\n");
      break;
    }
    buffer.push_back(new_char);
    printf("%c", new_char);
  }

  return buffer;
}

void SnapcastPlayer::control_rpc_version_() {
  std::string control_rpc_version_message = json::build_json([](JsonObject root) {
    root["id"] = 8;
    root["jsonrpc"] = "2.0";
    root["method"] = "Server.GetRPCVersion";
  });

  control_rpc_version_message.push_back('\n');

  this->control_socket_->write((void *) control_rpc_version_message.data(), control_rpc_version_message.size());
  std::string response = this->read_until_newline_(this->control_socket_.get());
  ESP_LOGD(TAG, "control_rpc_version_message: %s", response.c_str());
}

void SnapcastPlayer::control_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  xEventGroupWaitBits(this_snapcast->event_group_, EventGroupBits::CONTROL_START, true, false, portMAX_DELAY);
  while (true) {
    std::string notification = this_snapcast->read_until_newline_(this_snapcast->control_socket_.get());
    printf("Control task received a notification %s\n", notification.c_str());

    bool valid = json::parse_json(notification, [this_snapcast](JsonObject root) -> bool {
      if (!root.containsKey("jsonrpc")) {
        ESP_LOGE(TAG, "JSON RPC notification isn't valid");
        return false;
      }
      std::string method = root["method"].as<std::string>();
      printf("method: %s\n", method.c_str());
      return true;
    });
    delay(10);
  }
}

void SnapcastPlayer::snapcast_task(void *params) {  // // Find snapcast server
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  esp_timer_handle_t timesync_message_timer;
  static const esp_timer_create_args_t timer_for_syncing_args = {.callback = &timesync_callback,
                                                                 .arg = (void *) this_snapcast,
                                                                 .dispatch_method = ESP_TIMER_TASK,
                                                                 .name = "time_sync",
                                                                 .skip_unhandled_events = false};
  // create a timer to send time sync messages every x µs
  esp_timer_create(&timer_for_syncing_args, &timesync_message_timer);

  while (true) {
    this_snapcast->connected_ = false;
    esp_timer_stop(timesync_message_timer);

    if (this_snapcast->connect_to_server_() != ESP_OK) {
      ESP_LOGW(TAG, "Failed to connect to snapcast server, retrying in 5 seconds\n");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (this_snapcast->send_hello_message_() != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send the hello message, trying in 5 seconds.");
      this_snapcast->client_socket_->shutdown(0);
      this_snapcast->client_socket_->close();
      this_snapcast->control_socket_->shutdown(0);
      this_snapcast->control_socket_->close();
      continue;
    }

    this_snapcast->connected_ = true;

    bool low_speed_timer_started = false;
    bool high_speed_timer_started = false;

    bool new_file_start = true;

    AudioSyncChunk *audio_chunk = chunk_allocator.allocate(1);

    if (audio_chunk == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate audio chunk");
      this_snapcast->client_socket_->shutdown(0);
      this_snapcast->client_socket_->close();
      this_snapcast->control_socket_->shutdown(0);
      this_snapcast->control_socket_->close();
      continue;
    }

    int64_t last_time_sync_message = 0;
    uint32_t time_message_delay = FAST_SYNC_LATENCY_BUF;

    int64_t last_client_settings_message = 0;
    uint32_t client_message_delay = 60000000;

    bool no_socket_error = true;

    while (true) {
      bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

      ssize_t bytes_read = this_snapcast->read_from_socket_(this_snapcast->client_socket_.get(),
                                                            base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);
      if (bytes_read == -1) {
        no_socket_error = false;
        ESP_LOGE(TAG, "Failed to read from the socket, closing the connection.");
        this_snapcast->client_socket_->shutdown(0);
        this_snapcast->client_socket_->close();
        this_snapcast->control_socket_->shutdown(0);
        this_snapcast->control_socket_->close();
        break;
      }

      int64_t now = esp_timer_get_time();

      BaseMessage base_msg;
      this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);

      base_msg.received.sec = static_cast<int32_t>(now / 1000000LL);
      base_msg.received.usec = static_cast<int32_t>(now - now / 1000000LL);

      size_t offset = 0;
      size_t base_msg_size = base_msg.size;
      if (base_msg_size > MAX_CHUNK_SIZE) {
        ESP_LOGE(TAG,
                 "message size is bigger than the max chunk size, problematic! Message size = %" PRIu32
                 ". Message type = %d",
                 base_msg_size, base_msg.type);
        continue;
      }
      while (base_msg_size > 0) {
        ssize_t bytes_read = this_snapcast->client_socket_->read(audio_chunk->data + offset, base_msg_size);
        if (bytes_read == -1) {
          no_socket_error = false;
          break;
        }
        base_msg_size -= bytes_read;
        offset += bytes_read;
      }
      audio_chunk->offset = 0;
      audio_chunk->size = base_msg.size;

      if (!no_socket_error) {
        ESP_LOGE(TAG, "Failed to read from the socket, closing the connection.");
        this_snapcast->client_socket_->shutdown(0);
        this_snapcast->client_socket_->close();
        this_snapcast->control_socket_->shutdown(0);
        this_snapcast->control_socket_->close();
        break;
      }

      if (high_speed_timer_started && this_snapcast->network_latency_filter_.is_full()) {
        high_speed_timer_started = false;
        low_speed_timer_started = true;
        time_message_delay = NORMAL_SYNC_LATENCY_BUF;
        esp_timer_stop(timesync_message_timer);
        if (!esp_timer_is_active(timesync_message_timer)) {
          esp_timer_start_periodic(timesync_message_timer, time_message_delay);
        }
      }

      switch (base_msg.type) {
        case SNAPCAST_MESSAGE_CODEC_HEADER: {
          ESP_LOGD(TAG, "Received a new codec header message.");

          uint32_t codec_len = *reinterpret_cast<uint32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(uint32_t);
          audio_chunk->size -= sizeof(uint32_t);

          std::string codec_type;
          codec_type.resize(codec_len);
          std::memcpy((void *) codec_type.data(), (void *) (audio_chunk->data + audio_chunk->offset), codec_len);

          audio_chunk->offset += codec_len;
          audio_chunk->size -= codec_len;

          codec_len = *reinterpret_cast<uint32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(uint32_t);
          audio_chunk->size -= sizeof(uint32_t);

          if (codec_len != audio_chunk->size) {
            ESP_LOGE(TAG, "Codec length doesn't match base size! Base size = %d; codec header = %d", audio_chunk->size,
                     codec_len);
            no_socket_error = false;
            break;
          }

          audio_chunk->server_timestamp = 0;
          audio_chunk->codec_header = true;
          if (codec_len > 0) {
            // this_snapcast->speaker_->stop();
            xEventGroupSetBits(this_snapcast->event_group_, COMMAND_STOP);

            new_file_start = true;
            xQueueReset(this_snapcast->encoded_chunk_data_queue_);
            xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, portMAX_DELAY);

            if (this_snapcast->decode_task_handle_ == nullptr) {
              xTaskCreate(decode_task, "decode", DECODE_TASK_STACK_SIZE, (void *) this_snapcast, 1,
                          &this_snapcast->decode_task_handle_);
            }
          }

          if (!low_speed_timer_started) {
            time_message_delay = FAST_SYNC_LATENCY_BUF;
            esp_timer_stop(timesync_message_timer);
            if (!esp_timer_is_active(timesync_message_timer)) {
              esp_timer_start_periodic(timesync_message_timer, time_message_delay);
            }
            high_speed_timer_started = true;
          }
          break;
        }
        case SNAPCAST_MESSAGE_WIRE_CHUNK: {
          bool valid_chunk = this_snapcast->audio_stream_info_.has_value();

          int32_t timestamp_s = *reinterpret_cast<int32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(int32_t);
          audio_chunk->size -= sizeof(int32_t);
          int32_t timestamp_us = *reinterpret_cast<int32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(int32_t);
          audio_chunk->size -= sizeof(int32_t);
          uint32_t chunk_size = *reinterpret_cast<uint32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(uint32_t);
          audio_chunk->size -= sizeof(uint32_t);

          if (chunk_size != audio_chunk->size) {
            ESP_LOGE(TAG, "Wire chunk size doesn't match base size! Base size = %d; wire chunk = %d", audio_chunk->size,
                     chunk_size);
            no_socket_error = false;
            break;
          }

          int64_t total_timestamp_us =
              static_cast<int64_t>(timestamp_s) * 1000000LL + static_cast<int64_t>(timestamp_us);

          audio_chunk->codec_header = false;
          audio_chunk->server_timestamp = total_timestamp_us;
          if (chunk_size > 0) {
            if (low_speed_timer_started && valid_chunk && no_socket_error) {
              if (!xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, 0)) {
                ESP_LOGW(TAG, "Encoded chunk queue is full, dropping audio chunk.");
              }
            }
          }

          break;
        }
        case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
          uint32_t server_settings_len = *reinterpret_cast<uint32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(uint32_t);
          audio_chunk->size -= sizeof(uint32_t);

          if (server_settings_len != audio_chunk->size) {
            ESP_LOGE(TAG,
                     "Server settings message size doesn't match base size! Base size = %d; server settings size = %d",
                     audio_chunk->size, server_settings_len);
            no_socket_error = false;
            break;
          }

          if (server_settings_len > 0) {
            std::string server_msg_read_data;
            server_msg_read_data.resize(server_settings_len);
            std::memcpy((void *) server_msg_read_data.data(), (void *) (audio_chunk->data + audio_chunk->offset),
                        server_settings_len);

            ServerSettingsMessage server_settings_msg;
            this_snapcast->server_settings_message_deserialize_(&server_settings_msg, server_msg_read_data.c_str());

            printf("Server settings json: %s\n Server settings buffer ms: %d\n latency: %d\n muted: %d\n volume %d\n",
                   server_msg_read_data.c_str(), server_settings_msg.buffer_ms, server_settings_msg.latency,
                   server_settings_msg.muted, server_settings_msg.volume);

            this_snapcast->snapcast_buffer_duration_ms_ = server_settings_msg.buffer_ms;
            if (server_settings_msg.latency != this_snapcast->snapcast_latency_ms_) {
              this_snapcast->actual_offsets_.reset();
            }
            this_snapcast->snapcast_latency_ms_ = server_settings_msg.latency;

            this_snapcast->volume_ = server_settings_msg.volume;
            this_snapcast->speaker_->set_mute_state(server_settings_msg.muted);
            this_snapcast->external_mute_ = server_settings_msg.muted;
          }

          break;
        }
        case SNAPCAST_MESSAGE_TIME: {
          int32_t latency_s = *reinterpret_cast<int32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(int32_t);
          audio_chunk->size -= sizeof(int32_t);
          int32_t latency_us = *reinterpret_cast<int32_t *>(audio_chunk->data + audio_chunk->offset);
          audio_chunk->offset += sizeof(int32_t);
          audio_chunk->size -= sizeof(int32_t);

          const int64_t latency_client_to_server_us =
              static_cast<int64_t>(latency_s) * 1000000LL + static_cast<int64_t>(latency_us);

          const int64_t time_rx_us = now;
          const int64_t time_tx_us =
              static_cast<int64_t>(base_msg.sent.sec) * 1000000LL + static_cast<int64_t>(base_msg.sent.usec);

          const int64_t latency_server_to_client_us = time_rx_us - time_tx_us;

          const int64_t network_latency_us = (latency_client_to_server_us - latency_server_to_client_us) / 2;

          this_snapcast->network_latency_filter_.update(network_latency_us);

          break;
        }

        default:

          break;
      }

      static uint32_t high_water_mark = 8192;
      uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
      if (new_high_water_mark < high_water_mark) {
        ESP_LOGD(TAG, "Snapcast task - High water mark increased from %d to %d.", high_water_mark, new_high_water_mark);
        high_water_mark = new_high_water_mark;
      }

      if (!no_socket_error) {
        ESP_LOGD(TAG, "Failed to read from the socket, closing the connection.");
        this_snapcast->client_socket_->shutdown(0);
        this_snapcast->client_socket_->close();
        this_snapcast->control_socket_->shutdown(0);
        this_snapcast->control_socket_->close();
        break;
      }
    }
  }
  while (true) {
    delay(10);
  }
}

void SnapcastPlayer::decode_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  AudioSyncChunk *encoded_chunk = chunk_allocator.allocate(1);
  AudioSyncChunk *decoded_chunk = chunk_allocator.allocate(1);
  std::unique_ptr<esp_audio_libs::flac::FLACDecoder> flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();

  while (true) {
    EventBits_t event_bits = xEventGroupGetBits(this_snapcast->event_group_);
    if ((event_bits & COMMAND_STOP) && !(event_bits & DECODE_FINISHED)) {
      // if (flac_decoder != nullptr) {
      //   flac_decoder.reset();
      // }
      // flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();

      xQueueReset(this_snapcast->decoded_chunk_data_queue_);

      xEventGroupSetBits(this_snapcast->event_group_, DECODE_FINISHED);
    }

    if (event_bits & DECODE_FINISHED) {
      delay(20);
      continue;
    }

    if (xQueueReceive(this_snapcast->encoded_chunk_data_queue_, encoded_chunk, pdMS_TO_TICKS(20))) {
      if (encoded_chunk->codec_header) {
        auto result = flac_decoder->read_header(encoded_chunk->data + encoded_chunk->offset, encoded_chunk->size);

        if (result == esp_audio_libs::flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
          printf("Need more data to decode FLAC header\n");
          continue;
        }

        if (result != esp_audio_libs::flac::FLAC_DECODER_SUCCESS) {
          printf("Serious error decoding flac header\n");
          continue;
        }

        size_t free_buffer_required = flac_decoder->get_output_buffer_size_bytes();

        this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(
            flac_decoder->get_sample_depth(), flac_decoder->get_num_channels(), flac_decoder->get_sample_rate());

        if (this_snapcast->sync_task_handle_ == nullptr) {
          xTaskCreate(sync_task, "sync", SYNC_TASK_STACK_SIZE, (void *) this_snapcast, 1,
                      &this_snapcast->sync_task_handle_);
        }

        this_snapcast->speaker_->set_audio_stream_info(this_snapcast->audio_stream_info_.value());

      } else if (flac_decoder != nullptr) {
        uint32_t output_samples = 0;
        auto result = flac_decoder->decode_frame(encoded_chunk->data + encoded_chunk->offset, encoded_chunk->size,
                                                 reinterpret_cast<int16_t *>(decoded_chunk->data), &output_samples);

        if (result == esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
          printf("flac decoder ran out of a data\n");
          continue;
        }

        if (result > esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
          printf(" more serious flac decoder error\n");
          continue;
        }

        size_t new_bytes = this_snapcast->audio_stream_info_.value().samples_to_bytes(output_samples);
        decoded_chunk->size = new_bytes;

        decoded_chunk->codec_header = false;
        decoded_chunk->server_timestamp = encoded_chunk->server_timestamp;
        xQueueSend(this_snapcast->decoded_chunk_data_queue_, decoded_chunk, pdMS_TO_TICKS(20));

        const uint32_t new_frames = this_snapcast->audio_stream_info_.value().bytes_to_frames(new_bytes);
        const uint32_t new_duration_us = this_snapcast->audio_stream_info_.value().frames_to_microseconds(new_frames);
      }
    }
    static uint32_t high_water_mark = 8192;
    uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
    if (new_high_water_mark < high_water_mark) {
      ESP_LOGD(TAG, "Decode task - High water mark increased from %d to %d.", high_water_mark, new_high_water_mark);
      high_water_mark = new_high_water_mark;
    }
  }
}

void SnapcastPlayer::sync_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  AudioSyncChunk *chunk = chunk_allocator.allocate(1);

  std::deque<AudioSyncChunkTimings> chunk_timings;

  while (true) {
    std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
        audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
    output_transfer_buffer->set_sink(this_snapcast->speaker_);
    bool run_once = false;
    uint8_t synced_chunks = 0;

    int64_t pending_frame_corrections = 0;

    while (true) {
      static uint32_t high_water_mark = 8192;
      uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
      if (new_high_water_mark < high_water_mark) {
        ESP_LOGD(TAG, "Sync task - High water mark increased from %d to %d.", high_water_mark, new_high_water_mark);
        high_water_mark = new_high_water_mark;
      }

      EventBits_t event_bits = xEventGroupGetBits(this_snapcast->event_group_);

      if (event_bits & SYNC_FINISHED) {
        delay(20);
        continue;
      }
      if (event_bits & COMMAND_STOP) {
        // clear things we own as well
        output_transfer_buffer.reset();
        output_transfer_buffer = audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
        output_transfer_buffer->set_sink(this_snapcast->speaker_);

        this_snapcast->speaker_->stop();

        this_snapcast->actual_offsets_.reset();
        pending_frame_corrections = 0;
        chunk_timings.clear();

        xEventGroupSetBits(this_snapcast->event_group_, SYNC_FINISHED);
        continue;
      }

      output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));

      PlaybackInfo playback_info;
      while (xQueueReceive(this_snapcast->playback_info_queue_, &playback_info, 0) == pdTRUE) {
        if (!chunk_timings.empty()) {
          uint32_t frames_played = playback_info.frames_played;
          int64_t write_timestamp = playback_info.write_timestamp;

          bool new_chunk = false;
          int32_t accumulated_chunk_corrections = 0;
          AudioSyncChunkTimings front_chunk = chunk_timings.front();
          while (front_chunk.total_frames < frames_played) {
            frames_played -= front_chunk.total_frames;
            accumulated_chunk_corrections += front_chunk.frame_corrections;

            chunk_timings.pop_front();
            front_chunk = chunk_timings.front();

            new_chunk = true;
          }

          // Now we are in the middle of the current audio chunk

          int64_t full_precision_microseconds =
              (frames_played * 1000000LL) /
              static_cast<int64_t>(this_snapcast->audio_stream_info_.value().get_sample_rate());
          int64_t server_timestamp_finished = front_chunk.server_timestamp + full_precision_microseconds;
          int64_t equivalent_client_timestamp = this_snapcast->server_timestamp_to_client_(server_timestamp_finished);
          chunk_timings.front().total_frames -= frames_played;
          chunk_timings.front().server_timestamp = server_timestamp_finished;

          if (abs(accumulated_chunk_corrections) > 10) {
            // Very large change, our median filter will be slow to a adapt
            this_snapcast->actual_offsets_.reset();
          }
          pending_frame_corrections -= accumulated_chunk_corrections;

          int64_t new_error = equivalent_client_timestamp - write_timestamp;

          this_snapcast->actual_offsets_.update(new_error);
        }
      }

      if (!xQueuePeek(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20))) {
        continue;
      }

      // if (chunk->size > output_transfer_buffer->free()) {
      //   continue;
      // }

      if (this_snapcast->speaker_->is_stopped()) {
        this_snapcast->speaker_->start();
      }

      if (!chunk_timings.empty()) {
        int64_t signed_pending_duration_corrections =
            (pending_frame_corrections * 1000000L) /
            static_cast<int64_t>(this_snapcast->audio_stream_info_.value().get_sample_rate());

        int64_t front_chunk_plays_at =
            this_snapcast->server_timestamp_to_client_(chunk_timings.front().server_timestamp);
        int64_t us_to_start = front_chunk_plays_at - esp_timer_get_time();
        if (us_to_start - signed_pending_duration_corrections > 200000) {
          this_snapcast->speaker_->set_pause_state(true);
          uint32_t pause_time_ms = us_to_start / 2000;
          printf("Hard sync: chunk doesn't play for %" PRId64 "ms, so pausing for %d ms\n", us_to_start / 1000,
                 pause_time_ms);
          vTaskDelay(pdMS_TO_TICKS(pause_time_ms));
          this_snapcast->speaker_->set_pause_state(false);
        }
      }

      int64_t signed_pending_duration_corrections =
          (pending_frame_corrections * 1000000L) /
          static_cast<int64_t>(this_snapcast->audio_stream_info_.value().get_sample_rate());

      int64_t recent_error_us = 0;

      if (this_snapcast->actual_offsets_.is_full()) {
        recent_error_us = this_snapcast->actual_offsets_.get_most_recent_median() - signed_pending_duration_corrections;
        if (abs(recent_error_us) < 5000) {
          synced_chunks = std::min(synced_chunks + 1, 10);
        } else {
          synced_chunks = 0;
        }
      }

      if ((synced_chunks < 10) && (!this_snapcast->speaker_->get_mute_state())) {
        printf("Hard sync: muting while until synced\n");
        this_snapcast->speaker_->set_mute_state(true);
      } else if ((synced_chunks >= 10) &&
                 (this_snapcast->external_mute_ != this_snapcast->speaker_->get_mute_state())) {
        printf("Successfully synced, setting to external mute state\n");
        this_snapcast->speaker_->set_mute_state(this_snapcast->external_mute_);
      }

      if (chunk->size > output_transfer_buffer->free()) {
        continue;
      } else {
        xQueueReceive(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20));
      }

      std::memcpy(output_transfer_buffer->get_buffer_end(), chunk->data, chunk->size);
      output_transfer_buffer->increase_buffer_length(chunk->size);

      uint32_t chunk_frame_count = this_snapcast->audio_stream_info_.value().bytes_to_frames(chunk->size);
      int32_t frame_corrections = 0;

      const size_t bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

      if (recent_error_us > 5000) {
        size_t silence_bytes = this_snapcast->audio_stream_info_.value().ms_to_bytes((recent_error_us - 25) / 1000);
        size_t actual_silence_bytes = std::min(silence_bytes, output_transfer_buffer->free());
        std::memset((void *) (output_transfer_buffer->get_buffer_end() - chunk->size), 0,
                    actual_silence_bytes + chunk->size);
        output_transfer_buffer->increase_buffer_length(actual_silence_bytes);
        frame_corrections = this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_silence_bytes);

        printf("Hard sync: adding %d frames of silence to hard sync. Current error is %" PRId64 "us\n",
               frame_corrections, recent_error_us);

      } else if (recent_error_us < -5000) {
        size_t bytes_to_remove =
            this_snapcast->audio_stream_info_.value().ms_to_bytes((abs(recent_error_us) - 25) / 1000);
        size_t actual_bytes_to_remove = std::min(bytes_to_remove, chunk->size - bytes_per_frame);
        output_transfer_buffer->decrease_buffer_length(actual_bytes_to_remove);
        frame_corrections = -this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_bytes_to_remove);
        printf("Hard sync: removing %d frames from a chunk. Current error is % " PRId64 "us\n", frame_corrections,
               recent_error_us);

      } else if (recent_error_us < -25) {
        const uint32_t num_channels = this_snapcast->audio_stream_info_.value().get_channels();
        int16_t *samples = reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
        for (int chan = 0; chan < num_channels; ++chan) {
          const int16_t left_sample = samples[chan];
          const int16_t right_sample = samples[num_channels + chan];
          samples[chan] = left_sample / 2 + right_sample / 2;
        }
        output_transfer_buffer->decrease_buffer_length(bytes_per_frame);
        frame_corrections = -1;
      } else if (recent_error_us > 25) {
        if (output_transfer_buffer->free() >= bytes_per_frame) {
          const uint32_t num_channels = this_snapcast->audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            const int16_t inserted_sample = left_sample / 2 + right_sample / 2;
            samples[num_channels + chan] = inserted_sample;
            samples[2 * num_channels + chan] = right_sample;
          }
          output_transfer_buffer->increase_buffer_length(bytes_per_frame);
          frame_corrections = 1;
        }
      }

      chunk_frame_count += frame_corrections;
      pending_frame_corrections += frame_corrections;

      AudioSyncChunkTimings timings;
      timings.server_timestamp = chunk->server_timestamp;
      timings.internal_timestamp = esp_timer_get_time();
      timings.total_frames = chunk_frame_count;
      timings.frame_corrections = frame_corrections;
      chunk_timings.push_back(timings);
    }
  }
}

int64_t SnapcastPlayer::server_timestamp_to_client_(int64_t server_timestamp) {
  return server_timestamp - this->network_latency_filter_.get_most_recent_median() +
         (this->snapcast_buffer_duration_ms_ - this->snapcast_latency_ms_) * 1000;
}

void SnapcastPlayer::base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  buffer.put_uint16(msg->type);
  buffer.put_uint16(msg->id);
  buffer.put_uint16(msg->refers_to);
  buffer.put_int32(msg->sent.sec);
  buffer.put_int32(msg->sent.usec);
  buffer.put_int32(msg->received.sec);
  buffer.put_int32(msg->received.usec);
  buffer.put_uint32(msg->size);
}

void SnapcastPlayer::base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  msg->type = buffer.get_uint16();
  msg->id = buffer.get_uint16();
  msg->refers_to = buffer.get_uint16();
  msg->sent.sec = buffer.get_int32();
  msg->sent.usec = buffer.get_int32();
  msg->received.sec = buffer.get_int32();
  msg->received.usec = buffer.get_int32();
  msg->size = buffer.get_uint32();
}

std::string SnapcastPlayer::hello_message_serialize_() {
  HelloMessage hello_message;
  hello_message.mac = this->player_id_.c_str();
  hello_message.hostname = App.get_name().c_str();
  hello_message.version = "0.0.1";
  hello_message.client_name = "esphome";
  hello_message.os = "esp32";
  hello_message.arch = "xtensa";
  hello_message.instance = 1;
  hello_message.id = this->player_id_.c_str();
  hello_message.protocol_version = 2;
  return this->build_hello_message_(&hello_message);
}

std::string SnapcastPlayer::client_message_serialize_(ClientInfoMessage *msg) {
  return json::build_json([msg](JsonObject root) {
    root["volume"] = msg->volume;
    root["muted"] = msg->muted;
  });
}

std::string SnapcastPlayer::build_hello_message_(HelloMessage *msg) {
  return json::build_json([msg](JsonObject root) {
    root["MAC"] = msg->mac;
    root["HostName"] = msg->hostname;
    root["Version"] = msg->version;
    root["ClientName"] = msg->client_name;
    root["OS"] = msg->os;
    root["Arch"] = msg->arch;
    root["Instance"] = msg->instance;
    root["ID"] = msg->id;
    root["SnapStreamProtocolVersion"] = msg->protocol_version;
  });
}

bool SnapcastPlayer::server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str) {
  bool valid = json::parse_json(json_str, [msg](JsonObject root) -> bool {
    if (!root.containsKey("bufferMs") || !root.containsKey("latency") || !root.containsKey("muted") ||
        !root.containsKey("volume")) {
      ESP_LOGE(TAG, "Server settings message doesn't contain all the fields");
      return false;
    }
    msg->buffer_ms = root["bufferMs"].as<int32_t>();
    msg->latency = root["latency"].as<int32_t>();
    msg->volume = root["volume"].as<uint32_t>();
    msg->muted = root["muted"].as<uint32_t>();

    return true;
  });

  return valid;
}

}  // namespace snapcast
}  // namespace esphome
#endif
