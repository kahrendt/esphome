#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mdns.h"

#include <esp_timer.h>

#include <flac_decoder.h>
#include <wav_decoder.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

static const size_t INPUT_BUFFER_SIZE = 1024 * 50;
static const size_t OUTPUT_BUFFER_SIZE = 1024 * 10;

static const uint32_t ENCODED_CHUNK_QUEUE_SIZE = 100;

static const uint32_t FAST_SYNC_LATENCY_BUF = 10000;      // in µs
static const uint32_t NORMAL_SYNC_LATENCY_BUF = 1000000;  // in µs

static const size_t CONTROL_TASK_STACK_SIZE = 3 * 1024;
static const size_t SNAPCAST_TASK_STACK_SIZE = 3 * 1024;
static const size_t DECODE_TASK_STACK_SIZE = 3 * 1024;

static const int GOOD_SYNCS_BEFORE_UNMUTE = 2;
static const int64_t HARD_SYNC_THRESHOLD_US = 5000;

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),
  DECODE_FINISHED = (1 << 3),
  CONTROL_START = (1 << 7),
  WARNING_ENCODED_CHUNK_FULL = (1 << 11),
};

void SnapcastPlayer::start() {
  this->speaker_->add_audio_output_callback([this](uint32_t frames_played, int64_t write_timestamp) {
    PlaybackProgress playback_progress = {.frames_played = frames_played, .write_timestamp = write_timestamp};
    if (!xQueueSend(this->playback_progress_queue_, &playback_progress, 0)) {
      ESP_LOGE(TAG, "Playback info queue was full");
    }
  });

  this->encoded_chunk_data_queue_ = xQueueCreate(ENCODED_CHUNK_QUEUE_SIZE, sizeof(AudioChunk));

  this->playback_progress_queue_ = xQueueCreate(50, sizeof(PlaybackProgress));
  this->event_group_ = xEventGroupCreate();

  xTaskCreate(snapcast_task, "snapcast", SNAPCAST_TASK_STACK_SIZE, (void *) this, 5, &this->snapcast_task_handle_);
  xTaskCreate(control_task, "snap_control", CONTROL_TASK_STACK_SIZE, (void *) this, 1, &this->control_task_handle_);
}

void SnapcastPlayer::loop() {
  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  if ((event_bits & DECODE_FINISHED)) {
    this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    if (this->speaker_->is_stopped()) {
      xQueueReset(this->playback_progress_queue_);
      xEventGroupClearBits(this->event_group_, (COMMAND_STOP | DECODE_FINISHED));
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

  if (this->stream_is_idle_.has_value()) {
    if (this->stream_is_idle_.value()) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
    }
  }

  if (this->state != old_state) {
    this->publish_state();
    ESP_LOGD(TAG, "State changed to %s", media_player::media_player_state_to_string(this->state));
  }
}

esp_err_t SnapcastPlayer::send_client_message_() {
  if (!this->connected_) {
    return ESP_OK;
  }

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

    ESP_LOGD(TAG, "Looking for a snapcast service on network");
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
  this->control_get_server_status();
  // this->control_rpc_version_();
  // printf("received control rpc version\n");

  int nodelay = 1;
  if (this->client_socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
    ESP_LOGW(TAG, "Failed to turn on TCP_NODELAY, syncing may not be accurate");
    nodelay = 0;
  }

  return ESP_OK;
}

void SnapcastPlayer::disconnect_from_server_() {
  this->connected_ = false;

  this->client_socket_->shutdown(0);
  this->client_socket_->close();
  this->control_socket_->shutdown(0);
  this->control_socket_->close();

  this->stream_is_idle_.reset();
  this->group_id_ = "";
  this->player_id_ = "";
  this->stream_id_ = "";
}

media_player::MediaPlayerTraits SnapcastPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();

  traits.set_supports_pause(true);

  return traits;
}

void SnapcastPlayer::setup() {
  this->player_id_ = get_mac_address_pretty();
  this->start();
}

void SnapcastPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!this->is_ready()) {
    // Ignore any commands sent before the media player is setup
    return;
  }

  if (call.get_volume().has_value()) {
    // this->control_set_stream_volume_(round(call.get_volume().value() * 100.0f));
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
  if (!this->connected_) {
    return "";
  }
  std::string buffer;
  char new_char = ' ';
  while (new_char != '\n') {
    ssize_t bytes_read = socket->read((void *) &new_char, sizeof(new_char));
    if (bytes_read == -1) {
      ESP_LOGW(TAG, "Couldn't read from control socket");
      return "";
    }
    buffer.push_back(new_char);
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

void SnapcastPlayer::control_get_server_status() {
  std::string control_rpc_version_message = json::build_json([](JsonObject root) {
    root["id"] = 8;
    root["jsonrpc"] = "2.0";
    root["method"] = "Server.GetStatus";
  });

  control_rpc_version_message.push_back('\n');

  this->control_socket_->write((void *) control_rpc_version_message.data(), control_rpc_version_message.size());
}
void SnapcastPlayer::control_set_stream_volume_(int volume) {
  std::string control_stream_volume_message = json::build_json([this, volume](JsonObject root) {
    JsonObject params;
    params["id"] = this->stream_id_;
    params["property"] = "volume";
    params["value"] = volume;
    root["id"] = 1;
    root["jsonrpc"] = "2.0";
    root["method"] = "Stream.SetProperty";
    root["params"] = params;
  });

  control_stream_volume_message.push_back('\n');

  this->control_socket_->write((void *) control_stream_volume_message.data(), control_stream_volume_message.size());
}

void SnapcastPlayer::control_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  xEventGroupWaitBits(this_snapcast->event_group_, EventGroupBits::CONTROL_START, true, false, portMAX_DELAY);
  while (true) {
    std::string message = this_snapcast->read_until_newline_(this_snapcast->control_socket_.get());
    if (message.empty()) {
      delay(10);
    }

    if (!message.empty()) {
      ESP_LOGV(TAG, "Control task received a message: %s", message.c_str());
      bool valid = json::parse_json(message, [this_snapcast](JsonObject root) -> bool {
        if (!root.containsKey("jsonrpc")) {
          ESP_LOGE(TAG, "Control JSON RPC notification isn't valid");
          return false;
        }
        std::string method = "";
        if (root.containsKey("method")) {
          method = root["method"].as<std::string>();
        }

        std::string result = "";
        if (root.containsKey("result")) {
          JsonObject params = root["result"].as<JsonObject>();
          if (params.containsKey("server")) {
            JsonObject server = params["server"].as<JsonObject>();
            if (server.containsKey("groups")) {
              JsonArray groups = server["groups"].as<JsonArray>();
              for (const JsonObject &group : groups) {
                if (group.containsKey("clients")) {
                  JsonArray clients = group["clients"].as<JsonArray>();
                  for (const JsonObject &client : clients) {
                    if (client["id"].as<std::string>().compare(this_snapcast->player_id_) == 0) {
                      this_snapcast->group_id_ = group["id"].as<std::string>();
                      this_snapcast->stream_id_ = group["stream_id"].as<std::string>();
                      ESP_LOGV(TAG, "Found which group we are in, current group id is %s streaming %s",
                               this_snapcast->group_id_.c_str(), this_snapcast->stream_id_.c_str());
                    }
                  }
                }
              }
            }
          }
        }

        if (method.compare("Server.OnUpdate") == 0) {
          if (root.containsKey("params")) {
            JsonObject params = root["params"].as<JsonObject>();
            if (params.containsKey("server")) {
              JsonObject server = params["server"].as<JsonObject>();
              if (server.containsKey("groups")) {
                JsonArray groups = server["groups"].as<JsonArray>();
                for (const JsonObject &group : groups) {
                  if (group.containsKey("clients")) {
                    JsonArray clients = group["clients"].as<JsonArray>();
                    for (const JsonObject &client : clients) {
                      if (client["id"].as<std::string>().compare(this_snapcast->player_id_) == 0) {
                        this_snapcast->group_id_ = group["id"].as<std::string>();
                        this_snapcast->stream_id_ = group["stream_id"].as<std::string>();
                        ESP_LOGV(TAG, "Found which group we are in, current group id is %s streaming %s",
                                 this_snapcast->group_id_.c_str(), this_snapcast->stream_id_.c_str());
                      }
                    }
                  }
                }
              }
            }
          }
        }

        if (method.compare("Group.OnStreamChanged") == 0) {
          JsonObject group_stream_params = root["params"];
          if (group_stream_params["id"].as<std::string>().compare(this_snapcast->group_id_) == 0) {
            this_snapcast->stream_id_ = group_stream_params["stream_id"].as<std::string>();
            ESP_LOGV(TAG, "Current group changed stream id to %s", this_snapcast->stream_id_.c_str());
          }
        }
        if (method.compare("Stream.OnUpdate") == 0) {
          JsonObject stream_params = root["params"];
          if (stream_params["id"].as<std::string>().compare(this_snapcast->stream_id_) == 0) {
            std::string state = stream_params["stream"]["status"].as<std::string>();
            if (state.compare("idle") == 0) {
              this_snapcast->stream_is_idle_ = true;
            } else if (state.compare("playing") == 0) {
              this_snapcast->stream_is_idle_ = false;
            }
            ESP_LOGV(TAG, "Current stream state is %s", state.c_str());
          }
        }
        return true;
      });
    }
  }
}

void SnapcastPlayer::clear_chunk_queue_() {
  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);
  AudioChunk chunk;
  while (xQueueReceive(this->encoded_chunk_data_queue_, &chunk, pdMS_TO_TICKS(1))) {
    data_allocator.deallocate(chunk.data, chunk.offset + chunk.size);
  }
}

void SnapcastPlayer::snapcast_task(void *params) {  // // Find snapcast server
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);
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
      this_snapcast->disconnect_from_server_();
      continue;
    }

    this_snapcast->connected_ = true;

    bool low_speed_timer_started = false;
    bool high_speed_timer_started = false;

    bool new_file_start = true;

    AudioChunk audio_chunk;

    int64_t last_time_sync_message = 0;
    uint32_t time_message_delay = FAST_SYNC_LATENCY_BUF;

    bool no_socket_error = true;

    while (true) {
      bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

      ssize_t bytes_read = this_snapcast->read_from_socket_(this_snapcast->client_socket_.get(),
                                                            base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);
      if (bytes_read == -1) {
        no_socket_error = false;
        ESP_LOGE(TAG, "Failed to read from the socket, closing the connection.");
        this_snapcast->disconnect_from_server_();
      }

      int64_t now = esp_timer_get_time();

      BaseMessage base_msg;
      this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);

      base_msg.received.sec = static_cast<int32_t>(now / 1000000LL);
      base_msg.received.usec = static_cast<int32_t>(now - now / 1000000LL);

      size_t follow_up_msg_size = base_msg.size;

      uint8_t *follow_up_data = data_allocator.allocate(follow_up_msg_size);
      if (follow_up_data == nullptr) {
        no_socket_error = false;
        ESP_LOGE(TAG, "Problem reading from the socket, closing the connection.");
        this_snapcast->disconnect_from_server_();
        break;
      }

      bytes_read =
          this_snapcast->read_from_socket_(this_snapcast->client_socket_.get(), follow_up_data, follow_up_msg_size);
      if (bytes_read != follow_up_msg_size) {
        data_allocator.deallocate(follow_up_data, follow_up_msg_size);
        no_socket_error = false;
        ESP_LOGE(TAG, "Problem reading from the socket, closing the connection.");
        this_snapcast->disconnect_from_server_();
        break;
      }

      audio_chunk.data = follow_up_data;
      audio_chunk.offset = 0;
      audio_chunk.size = bytes_read;

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
        case SNAPCAST_MESSAGE_WIRE_CHUNK: {
          int32_t timestamp_s;
          int32_t timestamp_us;
          uint32_t chunk_size;

          std::memcpy((void *) &timestamp_s, (void *) audio_chunk.data, sizeof(int32_t));
          std::memcpy((void *) &timestamp_us, (void *) audio_chunk.data + sizeof(int32_t), sizeof(int32_t));
          std::memcpy((void *) &chunk_size, (void *) audio_chunk.data + 2 * sizeof(int32_t), sizeof(uint32_t));

          audio_chunk.offset = WIRE_CHUNK_HEADER_SIZE;
          audio_chunk.size -= audio_chunk.offset;

          if (chunk_size != audio_chunk.size) {
            ESP_LOGE(TAG, "Wire chunk size doesn't match base size! Base size = %d; wire chunk = %d; offset = %d",
                     audio_chunk.size, chunk_size, audio_chunk.offset);
            no_socket_error = false;
            break;
          }

          const int64_t total_timestamp_us =
              static_cast<int64_t>(timestamp_s) * 1000000LL + static_cast<int64_t>(timestamp_us);

          audio_chunk.codec_header = false;
          audio_chunk.server_timestamp = total_timestamp_us;
          if ((chunk_size > 0) && low_speed_timer_started && no_socket_error) {
            if (!xQueueSend(this_snapcast->encoded_chunk_data_queue_, &audio_chunk, 0)) {
              xEventGroupSetBits(this_snapcast->event_group_, WARNING_ENCODED_CHUNK_FULL);
              ESP_LOGW(TAG, "Encoded chunk queue is full, dropping audio chunk.");
            } else {
              xEventGroupClearBits(this_snapcast->event_group_, WARNING_ENCODED_CHUNK_FULL);
              follow_up_data = nullptr;
            }
          }

          break;
        }
        case SNAPCAST_MESSAGE_CODEC_HEADER: {
          ESP_LOGD(TAG, "Received a new codec header message.");

          uint32_t codec_len;
          std::string codec_type;

          std::memcpy((void *) &codec_len, (void *) audio_chunk.data + audio_chunk.offset, sizeof(uint32_t));
          audio_chunk.offset += sizeof(uint32_t);
          audio_chunk.size -= sizeof(uint32_t);

          codec_type.resize(codec_len);
          std::memcpy((void *) codec_type.data(), (void *) (audio_chunk.data + audio_chunk.offset), codec_len);
          audio_chunk.offset += codec_len;
          audio_chunk.size -= codec_len;

          if (codec_type.compare("flac") == 0) {
            this_snapcast->codec_format_ = SnapcastCodecFormat::SNAPCAST_CODEC_FLAC;
          } else if (codec_type.compare("pcm") == 0) {
            this_snapcast->codec_format_ = SnapcastCodecFormat::SNAPCAST_CODEC_PCM;
          } else {
            ESP_LOGE(TAG, "Unsupported codec type: %s", codec_type.c_str());
            no_socket_error = false;
            break;
          }

          std::memcpy((void *) &codec_len, (void *) audio_chunk.data + audio_chunk.offset, sizeof(uint32_t));
          audio_chunk.offset += sizeof(uint32_t);
          audio_chunk.size -= sizeof(uint32_t);

          if (codec_len != audio_chunk.size) {
            ESP_LOGE(TAG, "Codec length doesn't match base size! Base size = %d; codec header = %d", audio_chunk.size,
                     codec_len);
            no_socket_error = false;
            break;
          }

          audio_chunk.server_timestamp = 0;
          audio_chunk.codec_header = true;
          if (codec_len > 0) {
            xEventGroupSetBits(this_snapcast->event_group_, COMMAND_STOP);

            new_file_start = true;
            this_snapcast->clear_chunk_queue_();
            xQueueSend(this_snapcast->encoded_chunk_data_queue_, &audio_chunk, portMAX_DELAY);
            follow_up_data = nullptr;  // Don't deallocate the data at end of this loop

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
        case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
          ESP_LOGD(TAG, "Received a server settings message");
          uint32_t server_settings_len = *reinterpret_cast<uint32_t *>(audio_chunk.data + audio_chunk.offset);
          audio_chunk.offset += sizeof(uint32_t);
          audio_chunk.size -= sizeof(uint32_t);

          if (server_settings_len != audio_chunk.size) {
            ESP_LOGE(TAG,
                     "Server settings message size doesn't match base size! Base size = %d; server settings size = %d",
                     audio_chunk.size, server_settings_len);
            no_socket_error = false;

            break;
          }

          if (server_settings_len > 0) {
            std::string server_msg_read_data;
            server_msg_read_data.resize(server_settings_len);
            std::memcpy((void *) server_msg_read_data.data(), (void *) (audio_chunk.data + audio_chunk.offset),
                        server_settings_len);

            ServerSettingsMessage server_settings_msg;
            this_snapcast->server_settings_message_deserialize_(&server_settings_msg, server_msg_read_data.c_str());

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
          int32_t latency_s;
          int32_t latency_us;

          std::memcpy((void *) &latency_s, (void *) audio_chunk.data + audio_chunk.offset, sizeof(int32_t));
          audio_chunk.offset += sizeof(int32_t);
          audio_chunk.size -= sizeof(int32_t);
          std::memcpy((void *) &latency_us, (void *) audio_chunk.data + audio_chunk.offset, sizeof(int32_t));
          audio_chunk.offset += sizeof(int32_t);
          audio_chunk.size -= sizeof(int32_t);

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

      if (follow_up_data != nullptr) {
        data_allocator.deallocate(follow_up_data, follow_up_msg_size);
        follow_up_data = nullptr;
      }

      static uint32_t high_water_mark = 8192;
      uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
      if (new_high_water_mark < high_water_mark) {
        ESP_LOGV(TAG, "Snapcast task - High water mark changed from %d to %d.", high_water_mark, new_high_water_mark);
        high_water_mark = new_high_water_mark;
      }

      if (!no_socket_error) {
        ESP_LOGD(TAG, "Failed to read from the socket, closing the connection.");
        this_snapcast->disconnect_from_server_();
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

  AudioChunk encoded_chunk;
  std::unique_ptr<esp_audio_libs::flac::FLACDecoder> flac_decoder;
  std::unique_ptr<esp_audio_libs::wav_decoder::WAVDecoder> wav_decoder;
  size_t free_buffer_required = 8000;

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
  output_transfer_buffer->set_sink(this_snapcast->speaker_);

  int64_t pending_frame_corrections = 0;

  int synced_chunks = 0;

  std::deque<InternalAudioTiming> chunk_timings;

  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);

  bool initial_decode = true;

  while (true) {
    EventBits_t event_bits = xEventGroupGetBits(this_snapcast->event_group_);
    if ((event_bits & COMMAND_STOP) && !(event_bits & DECODE_FINISHED)) {
      this_snapcast->audio_stream_info_.reset();

      // clear things we own as well
      output_transfer_buffer.reset();
      output_transfer_buffer = audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
      output_transfer_buffer->set_sink(this_snapcast->speaker_);

      this_snapcast->speaker_->stop();

      this_snapcast->actual_offsets_.reset();
      pending_frame_corrections = 0;
      chunk_timings.clear();

      xEventGroupSetBits(this_snapcast->event_group_, DECODE_FINISHED);
    }

    if (event_bits & DECODE_FINISHED) {
      delay(20);
      continue;
    }

    const size_t bytes_written = output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));

    if (output_transfer_buffer->available() > 0) {
      continue;
    }
    // if (output_transfer_buffer->free() < free_buffer_required) {
    //   uint32_t frames_written = this_snapcast->audio_stream_info_.value().bytes_to_frames(bytes_written);
    //   uint32_t ms_written =
    //       this_snapcast->audio_stream_info_.value().frames_to_milliseconds_with_remainder(&frames_written) +
    //       this_snapcast->audio_stream_info_.value().frames_to_microseconds(frames_written) / 1000;
    //   delay(ms_written / 2);
    //   continue;
    // }

    /** Use the information from the speaker on frames played to update teh current error */

    PlaybackProgress playback_progress;
    while (xQueueReceive(this_snapcast->playback_progress_queue_, &playback_progress, 0) == pdTRUE) {
      initial_decode = false;  // Some sent audio chunks have been played by the speaker
      if (!chunk_timings.empty()) {
        uint32_t frames_played = playback_progress.frames_played;
        int64_t write_timestamp = playback_progress.write_timestamp;

        InternalAudioTiming *front_chunk = &chunk_timings.front();

        pending_frame_corrections -= front_chunk->frame_corrections;
        front_chunk->frame_corrections = 0;

        while (front_chunk->total_frames < frames_played) {
          frames_played -= front_chunk->total_frames;

          chunk_timings.pop_front();
          front_chunk = &chunk_timings.front();

          pending_frame_corrections -= front_chunk->frame_corrections;
          front_chunk->frame_corrections = 0;
        }

        // Now we are in the middle of the current audio chunk

        chunk_timings.front().total_frames -= frames_played;

        uint32_t unplayed_frames = chunk_timings.front().total_frames;

        int64_t unplayed_ms =
            this_snapcast->audio_stream_info_.value().frames_to_milliseconds_with_remainder(&unplayed_frames);
        int64_t unplayed_us =
            1000 * unplayed_ms + this_snapcast->audio_stream_info_.value().frames_to_microseconds(unplayed_frames);

        int64_t server_timestamp_finished = front_chunk->server_timestamp - unplayed_us;
        int64_t equivalent_client_timestamp = this_snapcast->server_timestamp_to_client_(server_timestamp_finished);

        int64_t new_error = equivalent_client_timestamp - write_timestamp;

        this_snapcast->actual_offsets_.update(new_error);
      }
    }

    /*******************/
    /*****Determine teh current error with pending correction */

    int64_t signed_pending_duration_corrections =
        (pending_frame_corrections * 1000000LL) /
        static_cast<int64_t>(this_snapcast->audio_stream_info_.value().get_sample_rate());

    // Takes into account the pending error
    int64_t recent_error_us =
        this_snapcast->actual_offsets_.get_most_recent_median() - signed_pending_duration_corrections;

    if (abs(this_snapcast->actual_offsets_.get_most_recent_median()) < HARD_SYNC_THRESHOLD_US) {
      synced_chunks = std::min(synced_chunks + 1, GOOD_SYNCS_BEFORE_UNMUTE);
    } else if (recent_error_us > HARD_SYNC_THRESHOLD_US) {
      // Even with the upcoming adjustments we are out of sync, reset the count
      synced_chunks = 0;
    }

    if ((synced_chunks < GOOD_SYNCS_BEFORE_UNMUTE) && (!this_snapcast->speaker_->get_mute_state())) {
      ESP_LOGV(TAG, "Out of sync, muting output until corrected");
      this_snapcast->speaker_->set_mute_state(true);
    } else if ((synced_chunks >= GOOD_SYNCS_BEFORE_UNMUTE) &&
               (this_snapcast->external_mute_ != this_snapcast->speaker_->get_mute_state())) {
      ESP_LOGV(TAG, "In sync with server, setting mute state to existing setting");
      this_snapcast->speaker_->set_mute_state(this_snapcast->external_mute_);
    }
    /******* */

    size_t bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

    if (xQueuePeek(this_snapcast->encoded_chunk_data_queue_, &encoded_chunk, pdMS_TO_TICKS(20))) {
      bool receive_chunk = true;
      if (encoded_chunk.codec_header) {
        if (this_snapcast->codec_format_ == SnapcastCodecFormat::SNAPCAST_CODEC_FLAC) {
          ESP_LOGD(TAG, "Decoding FLAC header");

          // Restart FLAC decoder
          flac_decoder.reset();
          flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();

          auto result = flac_decoder->read_header(encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size);

          if (result == esp_audio_libs::flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
            ESP_LOGW(TAG, "Need more data to decode FLAC header");
            continue;
          }

          if (result != esp_audio_libs::flac::FLAC_DECODER_SUCCESS) {
            ESP_LOGE(TAG, "Serious error decoding FLAC header");
            continue;
          }

          this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(
              flac_decoder->get_sample_depth(), flac_decoder->get_num_channels(), flac_decoder->get_sample_rate());

          bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

          free_buffer_required = flac_decoder->get_output_buffer_size_bytes();
          if (!output_transfer_buffer->reallocate(free_buffer_required + bytes_per_frame)) {
            ESP_LOGE(TAG, "Failed to reallocate buffer for decoding FLAC");
            continue;
          }
        } else if (this_snapcast->codec_format_ == SnapcastCodecFormat::SNAPCAST_CODEC_PCM) {
          ESP_LOGD(TAG, "Decoding WAV header");

          // Restart the WAV decoder
          wav_decoder.reset();
          wav_decoder = make_unique<esp_audio_libs::wav_decoder::WAVDecoder>();
          wav_decoder->reset();

          esp_audio_libs::wav_decoder::WAVDecoderResult result =
              wav_decoder->decode_header(encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size);

          if (result == esp_audio_libs::wav_decoder::WAV_DECODER_SUCCESS_IN_DATA) {
            this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(
                wav_decoder->bits_per_sample(), wav_decoder->num_channels(), wav_decoder->sample_rate());
            bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);
          } else {
            ESP_LOGE(TAG, "Failed to parse WAV header");
            continue;
          }
        }
        this_snapcast->speaker_->set_audio_stream_info(this_snapcast->audio_stream_info_.value());
        initial_decode = true;
      } else {
        size_t new_bytes;
        if ((flac_decoder != nullptr) && (this_snapcast->codec_format_ == SnapcastCodecFormat::SNAPCAST_CODEC_FLAC)) {
          uint32_t output_samples = 0;
          auto result = flac_decoder->decode_frame(
              encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size,
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), &output_samples);

          if (result == esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
            ESP_LOGE(TAG, "FLAC decoder ran out of data");
            continue;
          }

          if (result > esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
            ESP_LOGE(TAG, "Serious error decoding FLAC file");
            continue;
          }
          new_bytes = this_snapcast->audio_stream_info_.value().samples_to_bytes(output_samples);
        } else if ((wav_decoder != nullptr) &&
                   (this_snapcast->codec_format_ == SnapcastCodecFormat::SNAPCAST_CODEC_PCM)) {
          if (output_transfer_buffer->capacity() < encoded_chunk.size + bytes_per_frame) {
            if (!output_transfer_buffer->reallocate(encoded_chunk.size + bytes_per_frame)) {
              ESP_LOGE(TAG, "Failed to reallocate buffer for the PCM audio chunk");
              continue;
            }
          }
          std::memcpy((void *) output_transfer_buffer->get_buffer_end(),
                      (void *) (encoded_chunk.data + encoded_chunk.offset), encoded_chunk.size);
          new_bytes = encoded_chunk.size;
        }

        output_transfer_buffer->increase_buffer_length(new_bytes);

        uint32_t new_frames = this_snapcast->audio_stream_info_.value().bytes_to_frames(new_bytes);
        const uint32_t new_duration_ms =
            this_snapcast->audio_stream_info_.value().frames_to_milliseconds_with_remainder(&new_frames);
        const int64_t new_duration_us =
            new_duration_ms * 1000 + this_snapcast->audio_stream_info_.value().frames_to_microseconds(new_frames);

        int32_t frame_corrections = 0;

        const int64_t us_per_frame_margin = 3 * this_snapcast->audio_stream_info_.value().frames_to_microseconds(1) / 2;

        if (initial_decode || (recent_error_us > HARD_SYNC_THRESHOLD_US)) {
          size_t silence_bytes = this_snapcast->audio_stream_info_.value().ms_to_bytes(recent_error_us / 1000);
          size_t actual_silence_bytes = std::min(silence_bytes, output_transfer_buffer->free());
          std::memset((void *) (output_transfer_buffer->get_buffer_end() - new_bytes), 0,
                      actual_silence_bytes + new_bytes);
          output_transfer_buffer->increase_buffer_length(actual_silence_bytes);
          frame_corrections = this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_silence_bytes);

          ESP_LOGV(TAG,
                   "Hard sync: adding %" PRId32 " frames of silence. Current error is %" PRId64 "us. There are %" PRId64
                   "pending frames for correction",
                   frame_corrections, recent_error_us, pending_frame_corrections);
          receive_chunk = false;  // Don't actually process this frame since it was completely silenced

        } else if (recent_error_us < -HARD_SYNC_THRESHOLD_US) {
          size_t bytes_to_remove = this_snapcast->audio_stream_info_.value().ms_to_bytes(abs(recent_error_us) / 1000);
          size_t actual_bytes_to_remove = std::min(bytes_to_remove, new_bytes - bytes_per_frame);
          output_transfer_buffer->decrease_buffer_length(actual_bytes_to_remove);
          frame_corrections = -this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_bytes_to_remove);
          ESP_LOGV(TAG,
                   "Hard sync: removing %" PRId32 " frames. Current error is %" PRId64 "us. There are %" PRId64
                   "pending frames for correction",
                   frame_corrections, recent_error_us, pending_frame_corrections);
        } else if (recent_error_us < -us_per_frame_margin) {
          const uint32_t num_channels = this_snapcast->audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            samples[chan] = left_sample / 2 + right_sample / 2;
          }
          output_transfer_buffer->decrease_buffer_length(bytes_per_frame);
          frame_corrections = -1;
        } else if (recent_error_us > us_per_frame_margin) {
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

        InternalAudioTiming timings;

        timings.total_frames = this_snapcast->audio_stream_info_.value().bytes_to_frames(new_bytes) + frame_corrections;
        if (receive_chunk) {
          timings.server_timestamp = encoded_chunk.server_timestamp + new_duration_us;
          timings.frame_corrections = frame_corrections;
          pending_frame_corrections += frame_corrections;
        } else {
          timings.server_timestamp = encoded_chunk.server_timestamp;
          timings.frame_corrections = timings.total_frames;
          pending_frame_corrections += timings.total_frames;
        }

        chunk_timings.push_back(timings);

        // static int log_count = 0;
        // ++log_count;
        // if (log_count > 50) {
        //   printf("Current sync error: %" PRId64 "\n", this_snapcast->actual_offsets_.get_most_recent_median());
        //   log_count = 0;
        // }
      }
      if (receive_chunk) {
        xQueueReceive(this_snapcast->encoded_chunk_data_queue_, &encoded_chunk, portMAX_DELAY);
        data_allocator.deallocate(encoded_chunk.data, encoded_chunk.offset + encoded_chunk.size);
      }
    }
    static uint32_t high_water_mark = 8192;
    uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
    if (new_high_water_mark < high_water_mark) {
      ESP_LOGD(TAG, "Decode task - High water mark changed from %d to %d.", high_water_mark, new_high_water_mark);
      high_water_mark = new_high_water_mark;
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
