#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "esphome/components/media_player/media_player.h"
#include "esphome/components/socket/socket.h"

#include <freertos/queue.h>

#include "esp_check.h"

namespace esphome {
namespace snapcast {

class Snapcontrol {
 public:
  Snapcontrol(std::string player_id) : player_id_(player_id){};

  esp_err_t connect_to_server(std::string server_address = "", uint16_t port = 1705);
  void disconnect_from_server();

  esp_err_t process_messages();

  void control_snapcast_stream(media_player::MediaPlayerCommand command);
  void control_get_server_status();

  optional<bool> get_stream_is_idle() { return this->stream_is_idle_; }

  bool is_connected() { return this->is_connected_; }

 protected:
  void parse_snapcast_server_(JsonObject server);
  void parse_snapcast_groups_(JsonArray groups);
  void parse_snapcast_streams_(JsonArray streams);
  std::string read_until_newline_(socket::Socket *socket);

  std::unique_ptr<socket::Socket> control_socket_;

  bool is_connected_{false};
  std::string player_id_;
  std::string group_id_{""};
  std::string stream_id_{""};
  optional<bool> stream_is_idle_;

  std::string album_{""};
  std::string artist_{""};
  std::string track_{""};
};
}  // namespace snapcast
}  // namespace esphome
#endif