#include "snapclient.h"

#ifdef USE_NETWORK

#include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_timer.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast.client";

esp_err_t Snapclient::connect_to_server(std::string server_address, uint16_t port) {
  esp_err_t err = ESP_OK;

  socklen_t sl = 0;
  struct sockaddr_storage server;

  sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), server_address.c_str(), port);

  if (sl == 0) {
    ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
    return ESP_FAIL;
  }
  this->client_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);

  err = this->client_socket_->connect((struct sockaddr *) &server, sizeof(server));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to connect: errno %d", err);
    return ESP_FAIL;
  }

  int nodelay = 1;
  if (this->client_socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
    ESP_LOGW(TAG, "Failed to turn on TCP_NODELAY, syncing may be inaccurate");
    nodelay = 0;
  }

  this->is_connected_ = true;
  return ESP_OK;
}

void Snapclient::disconnect_from_server() {
  this->is_connected_ = false;
  this->client_socket_->shutdown(0);
  this->client_socket_->close();
}

esp_err_t Snapclient::send_client_message(float volume, bool muted) {
  if (!this->is_connected_) {
    return ESP_FAIL;
  }

  ClientInfoMessage client_msg = {.volume = static_cast<uint32_t>(volume * 100.0f), .muted = muted};
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

esp_err_t Snapclient::send_hello_message() {
  if (!this->is_connected_) {
    return ESP_FAIL;
  }

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

esp_err_t Snapclient::send_time_message() {
  if (!this->is_connected_) {
    return ESP_FAIL;
  }

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

esp_err_t Snapclient::read_base_message(BaseMessage *base_msg) {
  if (!this->is_connected_) {
    return ESP_FAIL;
  }

  bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

  ssize_t bytes_read =
      this->read_from_socket_(this->client_socket_.get(), base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);
  if (bytes_read == -1) {
    ESP_LOGE(TAG, "Failed to read from the socket");
    return ESP_FAIL;
  }

  int64_t now = esp_timer_get_time();

  this->base_message_deserialize_(base_msg, base_msg_buffer);
  base_msg->received.sec = static_cast<int32_t>(now / 1000000LL);
  base_msg->received.usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL);

  return ESP_OK;
}

ProcessMessageResponse Snapclient::process_messages(BaseMessage &base_msg, AudioChunk *audio_chunk) {
  if (!this->is_connected_) {
    return ProcessMessageResponse::ERROR_DISCONNECTED;
  }

  size_t bytes_read = this->read_from_socket_(this->client_socket_.get(), audio_chunk->data, base_msg.size);
  if (bytes_read != base_msg.size) {
    ESP_LOGE(TAG, "Error reading from socket");
    return ProcessMessageResponse::ERROR_SOCKET_READ;
  }

  audio_chunk->offset = 0;
  audio_chunk->size = bytes_read;

  ProcessMessageResponse response = ProcessMessageResponse::ERROR_FAILURE;

  switch (base_msg.type) {
    case SNAPCAST_MESSAGE_WIRE_CHUNK: {
      int32_t timestamp_s;
      int32_t timestamp_us;
      uint32_t chunk_size;

      std::memcpy((void *) &timestamp_s, (void *) audio_chunk->data, sizeof(int32_t));
      std::memcpy((void *) &timestamp_us, (void *) (audio_chunk->data + sizeof(int32_t)), sizeof(int32_t));
      std::memcpy((void *) &chunk_size, (void *) (audio_chunk->data + 2 * sizeof(int32_t)), sizeof(uint32_t));

      audio_chunk->offset = WIRE_CHUNK_HEADER_SIZE;
      audio_chunk->size -= audio_chunk->offset;

      if (chunk_size != audio_chunk->size) {
        ESP_LOGE(TAG, "Wire chunk size doesn't match base size! Base size = %d; wire chunk = %d; offset = %d",
                 audio_chunk->size, chunk_size, audio_chunk->offset);
        response = ProcessMessageResponse::ERROR_MISMATCHED_SIZES;
        break;
      }

      const int64_t total_timestamp_us =
          static_cast<int64_t>(timestamp_s) * 1000000LL + static_cast<int64_t>(timestamp_us);

      audio_chunk->codec_header = false;
      audio_chunk->server_timestamp = total_timestamp_us;
      if (chunk_size > 0) {
        response = ProcessMessageResponse::PROCESSED_WIRE_CHUNK;
      }

      break;
    }
    case SNAPCAST_MESSAGE_CODEC_HEADER: {
      uint32_t codec_len;
      std::string codec_type;

      std::memcpy((void *) &codec_len, (void *) (audio_chunk->data + audio_chunk->offset), sizeof(uint32_t));
      audio_chunk->offset += sizeof(uint32_t);
      audio_chunk->size -= sizeof(uint32_t);

      codec_type.resize(codec_len);
      std::memcpy((void *) codec_type.data(), (void *) (audio_chunk->data + audio_chunk->offset), codec_len);
      audio_chunk->offset += codec_len;
      audio_chunk->size -= codec_len;

      if (codec_type.compare("flac") == 0) {
        this->codec_format_ = SnapcastCodecFormat::SNAPCAST_CODEC_FLAC;
      } else if (codec_type.compare("pcm") == 0) {
        this->codec_format_ = SnapcastCodecFormat::SNAPCAST_CODEC_PCM;
        // } else if (codec_type.compare("opus") == 0) {
        //   this->codec_format_ = SnapcastCodecFormat::SNAPCAST_CODEC_OPUS;
      } else {
        ESP_LOGE(TAG, "Unsupported codec type: %s", codec_type.c_str());
        response = ProcessMessageResponse::ERROR_UNSUPPORTED_FORMAT;
        break;
      }

      std::memcpy((void *) &codec_len, (void *) (audio_chunk->data + audio_chunk->offset), sizeof(uint32_t));
      audio_chunk->offset += sizeof(uint32_t);
      audio_chunk->size -= sizeof(uint32_t);

      if (codec_len != audio_chunk->size) {
        ESP_LOGE(TAG, "Codec length doesn't match base size! Base size = %d; codec header = %d", audio_chunk->size,
                 codec_len);
        response = ProcessMessageResponse::ERROR_MISMATCHED_SIZES;
        break;
      }

      audio_chunk->server_timestamp = 0;
      audio_chunk->codec_header = true;
      if (codec_len > 0) {
        response = ProcessMessageResponse::PROCESSED_CODEC_HEADER;
      }

      break;
    }
    case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
      uint32_t server_settings_len = *reinterpret_cast<uint32_t *>(audio_chunk->data + audio_chunk->offset);
      audio_chunk->offset += sizeof(uint32_t);
      audio_chunk->size -= sizeof(uint32_t);

      if (server_settings_len != audio_chunk->size) {
        ESP_LOGE(TAG, "Server settings message size doesn't match base size! Base size = %d; server settings size = %d",
                 audio_chunk->size, server_settings_len);
        response = ProcessMessageResponse::ERROR_MISMATCHED_SIZES;
        break;
      }

      if (server_settings_len > 0) {
        std::string server_msg_read_data;
        server_msg_read_data.resize(server_settings_len);
        std::memcpy((void *) server_msg_read_data.data(), (void *) (audio_chunk->data + audio_chunk->offset),
                    server_settings_len);

        this->server_settings_message_deserialize_(&this->server_settings_message_, server_msg_read_data.c_str());
      }

      response = ProcessMessageResponse::PROCESSED_SERVER_SETTINGS;
      break;
    }
    case SNAPCAST_MESSAGE_TIME: {
      int32_t latency_s;
      int32_t latency_us;

      std::memcpy((void *) &latency_s, (void *) (audio_chunk->data + audio_chunk->offset), sizeof(int32_t));
      audio_chunk->offset += sizeof(int32_t);
      audio_chunk->size -= sizeof(int32_t);
      std::memcpy((void *) &latency_us, (void *) (audio_chunk->data + audio_chunk->offset), sizeof(int32_t));
      audio_chunk->offset += sizeof(int32_t);
      audio_chunk->size -= sizeof(int32_t);

      const int64_t latency_client_to_server_us =
          static_cast<int64_t>(latency_s) * 1000000LL + static_cast<int64_t>(latency_us);

      const int64_t time_rx_us =
          static_cast<int64_t>(base_msg.received.sec) * 1000000LL + static_cast<int64_t>(base_msg.received.usec);
      const int64_t time_tx_us =
          static_cast<int64_t>(base_msg.sent.sec) * 1000000LL + static_cast<int64_t>(base_msg.sent.usec);

      const int64_t latency_server_to_client_us = time_rx_us - time_tx_us;

      const int64_t network_latency_us = (latency_client_to_server_us - latency_server_to_client_us) / 2;

      this->network_latency_filter_.update(network_latency_us);

      response = ProcessMessageResponse::PROCESSED_TIME;
      break;
    }
    default:
      break;
  }

  return response;
}

int64_t Snapclient::server_timestamp_to_client(int64_t server_timestamp) {
  return server_timestamp - this->network_latency_filter_.get_most_recent_median() +
         (this->server_settings_message_.buffer_ms - this->server_settings_message_.latency) * 1000;
}

void Snapclient::base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  buffer.put_uint16(msg->type);
  buffer.put_uint16(msg->id);
  buffer.put_uint16(msg->refers_to);
  buffer.put_int32(msg->sent.sec);
  buffer.put_int32(msg->sent.usec);
  buffer.put_int32(msg->received.sec);
  buffer.put_int32(msg->received.usec);
  buffer.put_uint32(msg->size);
}

void Snapclient::base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  msg->type = buffer.get_uint16();
  msg->id = buffer.get_uint16();
  msg->refers_to = buffer.get_uint16();
  msg->sent.sec = buffer.get_int32();
  msg->sent.usec = buffer.get_int32();
  msg->received.sec = buffer.get_int32();
  msg->received.usec = buffer.get_int32();
  msg->size = buffer.get_uint32();
}

std::string Snapclient::client_message_serialize_(ClientInfoMessage *msg) {
  return json::build_json([msg](JsonObject root) {
    root["volume"] = msg->volume;
    root["muted"] = msg->muted;
  });
}

std::string Snapclient::hello_message_serialize_() {
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

std::string Snapclient::build_hello_message_(HelloMessage *msg) {
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

ssize_t Snapclient::read_from_socket_(socket::Socket *socket, uint8_t *buffer, size_t length) {
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

bool Snapclient::server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str) {
  bool valid = json::parse_json(json_str, [msg](JsonObject root) -> bool {
    if (!root["bufferMs"].is<JsonVariant>() || !root["latency"].is<JsonVariant>() || !root["muted"].is<JsonVariant>() ||
        !root["volume"].is<JsonVariant>()) {
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