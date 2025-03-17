#include "snapcontrol.h"

#ifdef USE_NETWORK

#include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/log.h"

static const char *TAG = "snapcast.control";

namespace esphome {
namespace snapcast {

esp_err_t Snapcontrol::connect_to_server(std::string server_address, uint16_t port) {
  // Connect to to the specified server
  esp_err_t err = ESP_OK;

  socklen_t sl = 0;
  struct sockaddr_storage server;

  sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), server_address.c_str(), port);

  if (sl == 0) {
    ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
    return ESP_FAIL;
  }
  this->control_socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);

  err = this->control_socket_->connect((struct sockaddr *) &server, sizeof(server));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to connect: errno %d", err);
    return ESP_FAIL;
  }

  int nodelay = 1;
  if (this->control_socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
    ESP_LOGW(TAG, "Failed to turn on TCP_NODELAY, syncing may be inaccurate");
    nodelay = 0;
  }

  this->is_connected_ = true;
  return ESP_OK;
}

void Snapcontrol::disconnect_from_server() {
  this->is_connected_ = false;
  this->control_socket_->shutdown(0);
  this->control_socket_->close();

  // Clear stream state
  this->stream_is_idle_.reset();
  this->stream_is_playing_.reset();

  // Clear various IDs of the snapclient
  this->group_id_.clear();
  this->player_id_.clear();
  this->stream_id_.clear();

  // Clear metadata
  this->album_.clear();
  this->artist_.clear();
  this->title_.clear();
  this->art_url_.clear();
}

esp_err_t Snapcontrol::process_messages() {
  std::string message;
  esp_err_t err = this->read_until_newline_(&message);
  if (err == ESP_FAIL) {
    return ESP_FAIL;
  }

  ESP_LOGV(TAG, "Control task received a message: %s", message.c_str());
  bool valid = json::parse_json(message, [this](JsonObject root) -> bool {
    if (!root["jsonrpc"].is<JsonVariant>()) {
      ESP_LOGE(TAG, "Control JSON RPC notification isn't valid");
      return false;
    }
    std::string method = "";
    if (root["method"].is<JsonVariant>()) {
      method = root["method"].as<std::string>();
    }

    std::string result = "";
    if (root["result"].is<JsonVariant>()) {
      JsonObject params = root["result"].as<JsonObject>();
      if (params["server"].is<JsonVariant>()) {
        JsonObject server = params["server"].as<JsonObject>();
        this->parse_snapcast_server_(server);
      }
    }

    if (method.compare("Server.OnUpdate") == 0) {
      if (root["params"].is<JsonVariant>()) {
        JsonObject params = root["params"].as<JsonObject>();
        if (params["server"].is<JsonVariant>()) {
          JsonObject server = params["server"].as<JsonObject>();
          this->parse_snapcast_server_(server);
        }
      }
    }

    if (method.compare("Group.OnStreamChanged") == 0) {
      JsonObject group_stream_params = root["params"];
      if (group_stream_params["id"].as<std::string>().compare(this->group_id_) == 0) {
        this->stream_id_ = group_stream_params["stream_id"].as<std::string>();
        ESP_LOGV(TAG, "Current group changed stream id to %s", this->stream_id_.c_str());
      }
    }
    if (method.compare("Stream.OnUpdate") == 0) {
      JsonObject stream = root["params"];
      // if (stream) {
      //   this->parse_snapcast_stream_(stream);
      // }
      if (stream["id"].as<std::string>().compare(this->stream_id_) == 0) {
        std::string state = stream["stream"]["status"].as<std::string>();
        if (state.compare("idle") == 0) {
          this->stream_is_idle_ = true;
        } else if (state.compare("playing") == 0) {
          this->stream_is_idle_ = false;
        }
        ESP_LOGV(TAG, "Current stream state is %s", state.c_str());
      }

      if (stream["properties"].is<JsonVariant>()) {
        JsonObject properties = stream["properties"];
        this->parse_snapcast_stream_properties_(properties);
      }
    }
    if (method.compare("Stream.OnProperties") == 0) {
      JsonObject stream = root["params"];
      if (stream["id"].as<std::string>().compare(this->stream_id_) == 0) {
        if (stream["properties"].is<JsonVariant>()) {
          JsonObject properties = stream["properties"];
          this->parse_snapcast_stream_properties_(properties);
        }
      }

      // if (stream_params["id"].as<std::string>().compare(this->stream_id_) == 0) {
      //   JsonObject metadata = stream_params["metadata"];

      //   if (metadata["album"].is<JsonVariant>()) {
      //     this->album_ = metadata["album"].as<std::string>();
      //   } else {
      //     this->album_ = "";
      //   }
      //   if (metadata["artist"].is<JsonVariant>()) {
      //     this->artist_ = metadata["artist"].as<std::string>();
      //   } else {
      //     this->artist_ = "";
      //   }
      //   if (metadata["track"].is<JsonVariant>()) {
      //     this->track_ = metadata["track"].as<std::string>();
      //   } else {
      //     this->track_ = "";
      //   }
      // }
    }
    return true;
  });
  return ESP_OK;
}

void Snapcontrol::control_get_server_status() {
  if (!this->is_connected_) {
    return;
  }
  std::string control_message = json::build_json([](JsonObject root) {
    root["id"] = 8;
    root["jsonrpc"] = "2.0";
    root["method"] = "Server.GetStatus";
  });

  control_message.push_back('\n');

  this->control_socket_->write((void *) control_message.data(), control_message.size());
}

void Snapcontrol::control_snapcast_stream(media_player::MediaPlayerCommand command) {
  if (!this->is_connected_) {
    return;
  }

  std::string snapcast_command = "";
  switch (command) {
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      snapcast_command = "play";
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
      snapcast_command = "pause";
      break;
    case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
      snapcast_command = "playPause";
      break;
    case media_player::MEDIA_PLAYER_COMMAND_STOP:
      snapcast_command = "stop";
      break;
    default:
      break;
  }

  std::string control_message = json::build_json([this, snapcast_command](JsonObject root) {
    root["id"] = 1;
    root["jsonrpc"] = "2.0";
    root["method"] = "Stream.Control";
    root["params"].to<JsonObject>();
    root["params"]["id"] = this->stream_id_;
    root["params"]["command"] = snapcast_command;
    root["params"]["params"].to<JsonObject>();
  });

  control_message.push_back('\n');
  ESP_LOGD(TAG, "Sending stream control message to snapserver: %s", control_message.c_str());
  this->control_socket_->write((void *) control_message.data(), control_message.size());
}

void Snapcontrol::parse_snapcast_server_(JsonObject server) {
  JsonArray groups = server["groups"].as<JsonArray>();
  if (groups.size() > 0) {
    this->parse_snapcast_groups_(groups);
  }

  JsonArray streams = server["streams"];
  if (streams.size() > 0) {
    this->parse_snapcast_streams_(streams);
  }
}

void Snapcontrol::parse_snapcast_groups_(JsonArray groups) {
  for (const JsonObject &group : groups) {
    if (group["clients"].is<JsonVariant>()) {
      JsonArray clients = group["clients"].as<JsonArray>();
      for (const JsonObject &client : clients) {
        if (client["id"].as<std::string>().compare(this->player_id_) == 0) {
          this->group_id_ = group["id"].as<std::string>();
          this->stream_id_ = group["stream_id"].as<std::string>();
          ESP_LOGV(TAG, "Found which group we are in, current group id is %s streaming %s", this->group_id_.c_str(),
                   this->stream_id_.c_str());
          break;
        }
      }
    }
  }
}

void Snapcontrol::parse_snapcast_streams_(JsonArray streams) {
  if (this->stream_id_.empty()) {
    return;
  }

  for (const JsonObject &stream : streams) {
    if (stream["id"].as<std::string>().compare(this->stream_id_) == 0) {
      std::string state = stream["status"].as<std::string>();
      if (state.compare("idle") == 0) {
        this->stream_is_idle_ = true;
      } else if (state.compare("playing") == 0) {
        this->stream_is_idle_ = false;
      }
      ESP_LOGV(TAG, "Determined current stream state %s", state.c_str());

      JsonObject properties = stream["properties"];
      if (properties) {
        this->parse_snapcast_stream_properties_(properties);
      }
      break;
    }
  }
}

void Snapcontrol::parse_snapcast_stream_properties_(JsonObject properties) {
  if (properties["playbackStatus"].is<std::string>()) {
    std::string playback_status = properties["playbackStatus"].as<std::string>();
    if (playback_status.compare("playing") == 0) {
      this->stream_is_playing_ = true;
    } else if (playback_status.compare("paused") == 0) {
      this->stream_is_playing_ = false;
    } else if (playback_status.compare("stopped") == 0) {
      this->stream_is_playing_ = false;
    } else if (playback_status.compare("unknown") == 0) {
      this->stream_is_playing_.reset();
    }

    ESP_LOGV(TAG, "Determined current stream playback status %s", playback_status.c_str());
  }

  if (properties["metadata"].is<JsonVariant>()) {
    JsonObject metadata = properties["metadata"];

    if (metadata["album"].is<JsonVariant>()) {
      this->album_ = metadata["album"].as<std::string>();
    } else {
      this->album_ = "";
    }
    if (metadata["artist"].is<JsonVariant>()) {
      this->artist_ = metadata["artist"].as<std::string>();
    } else {
      this->artist_ = "";
    }
    if (metadata["title"].is<JsonVariant>()) {
      this->title_ = metadata["title"].as<std::string>();
    } else {
      this->title_ = "";
    }
    if (metadata["artUrl"].is<JsonVariant>()) {
      this->art_url_ = metadata["artUrl"].as<std::string>();
    } else {
      this->art_url_ = "";
    }

    ESP_LOGV(TAG, "Received metadata: track title: %s", this->title_.c_str());
  }
}

esp_err_t Snapcontrol::read_until_newline_(std::string *buffer) {
  if (!this->is_connected_) {
    *buffer = "";
    return ESP_FAIL;
  }
  char new_char = ' ';
  while (new_char != '\n') {
    ssize_t bytes_read = this->control_socket_->read((void *) &new_char, sizeof(new_char));
    if (bytes_read == -1) {
      ESP_LOGW(TAG, "Couldn't read from control socket");
      *buffer = "";
      return ESP_FAIL;
    }
    buffer->push_back(new_char);
  }

  return ESP_OK;
}

}  // namespace snapcast
}  // namespace esphome
#endif
