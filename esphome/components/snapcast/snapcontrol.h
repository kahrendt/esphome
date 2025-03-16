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
  /* Class that communicates with the snapserver as a snapcontroller using a socket and the JSON-RPC API
   *
   * Attempts to find which group and stream the current player is
   *  - ``parse_snapcast_groups`` determines which group the player is in
   *  - ``parse_snapcast_streams`` determines which stream the group is playing
   *
   * 1) Connect to the snapserver via ``connect_to_server``
   * 2) Regularly call ``process_messages`` to monitor the state of the snapserver
   *
   * Call ``control_get_server_status`` to get the current server state
   * Call ``control_snapcast_stream`` to send commands to the current stream
   */
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
  std::string art_url_{""};
};
}  // namespace snapcast
}  // namespace esphome
#endif
