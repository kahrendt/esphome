#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "esphome/components/media_player/media_player.h"
#include "esphome/components/socket/socket.h"

#include <freertos/queue.h>
#include <vector>

#include "esp_check.h"

namespace esphome {
namespace snapcast {

struct Snapgroup {
  std::string name;
  std::string group_id;
  std::string stream_id;
  std::vector<std::string> clients;
  bool is_active;
};

class Snapcontrol {
  /* Class that communicates with the snapserver as a snapcontroller using a socket and the JSON-RPC API.
   *
   * Based on https://github.com/badaix/snapcast/blob/develop/doc/json_rpc_api/control.md (accessed 2025-03-18)
   *
   * Attempts to find which group and stream the current player is
   *  - ``parse_snapcast_groups`` determines which group the player is in
   *  - ``parse_snapcast_streams`` determines which stream the group is playing
   *
   * 1) Connect to the snapserver via ``connect_to_server``
   * 2) Regularly call ``process_messages`` to monitor the state of the snapserver
   *
   * Call ``request_server_status`` to get the current server state
   * Call ``control_stream`` to send commands to the current stream
   */
 public:
  Snapcontrol(std::string player_id) : player_id_(player_id){};

  /// @brief Connects to a snapserver's control tcp interface
  /// @param server_address (std::string) Server IP address
  /// @param port (uint16_t) Port for the snapcontrol tcp interface
  /// @return ESP_OK if successful, ESP_FAIL if there was a problem connecting the socket
  esp_err_t connect_to_server(std::string server_address, uint16_t port);

  /// @brief Disconnects and shuts down the socket. Resets ``current_group_id_``, ``current_stream_id_`` and metadata
  /// member variables.
  void disconnect_from_server();

  /// @brief Reads from the socket until a complete JSON-RPC message is received. Processes the following
  /// responses/notifications:
  ///  - Server.GetStatus (response) - determines groups, their streams, and their states
  ///  - Server.OnUpdate (notification) - determines groups, their streams, and their states
  ///  - Group.OnStreamChanged (notification) - updates the stream corresponding to each group
  ///  - Stream.OnUpdate (notification) - updates the playback state of each group
  ///  - Stream.OnProperties (notification) - determines the metadeta for the current group
  /// @return ESP_FAIL if there is an issue reading from the socket, ESP_OK otherwise
  esp_err_t process_messages();

  /// @brief Sends a command to control the current stream.
  /// @param command (MediaPlayerCommand) command to send
  void control_stream(media_player::MediaPlayerCommand command);

  /// @brief Sends a Server.GetStatus request. Reponse needs to be processed by calling ``process_messages``
  void request_server_status();

  /// @brief Gets if the current stream is status is idle.
  /// @return (optional<bool>) True if idle, false if playing, no value if state is unknown
  optional<bool> get_stream_is_idle() { return this->stream_is_idle_; }

  /// @brief Gets if the current stream's playbackStatus properties is playing
  /// Not all streams have this information
  /// @return (optional<bool>) True if playing, false if paused or stopped, no value if state is unknown
  optional<bool> get_stream_is_playing() { return this->stream_is_playing_; }

  /// @brief Tests if the socket is currently connected.
  /// @return True if there is an active connection, false otherwise
  bool is_connected() { return this->is_connected_; }

  /// @brief Experimental
  void join_another_group();

 protected:
  /// @brief Processes a Server.GetStatus response or Server.OnUpdate notification.
  /// Determines groups, their stream, and their streams' states.
  /// @param server (JsonObject)
  void parse_snapcast_server_(JsonObject server);

  /// @brief Processes all the listed groups.
  /// Determines groups and their streams along with identifying the player's current group id
  /// @param groups (JsonArray)
  void parse_snapcast_groups_(JsonArray groups);

  /// @brief Processes all the listed streams.
  /// Determines each streams status. Also parses any available properties for metadata and playbackStatus.
  /// @param streams  (JsonArray)
  void parse_snapcast_streams_(JsonArray streams);

  /// @brief Processes the properties JSON object of a stream JSON object.
  /// Determines the playbackStatus and metadata of the stream.
  /// @param properties (JsonObject)
  void parse_snapcast_stream_properties_(JsonObject properties);

  /// @brief Reads from the socket until receiving a newline character.
  /// Blocks while waiting for data
  /// @param buffer (std::string) to store the read data
  /// @return ESP_FAIL if there is a problem reading from the socket, ESP_OK otherwise
  esp_err_t read_until_newline_(std::string *buffer);

  std::unique_ptr<socket::Socket> socket_;

  std::vector<Snapgroup> snapgroups_;

  bool is_connected_{false};

  std::string player_id_;

  std::string current_group_id_{""};
  std::string current_stream_id_{""};
  optional<bool> stream_is_idle_;
  optional<bool> stream_is_playing_;

  std::string album_{""};
  std::string artist_{""};
  std::string title_{""};
  std::string art_url_{""};
};
}  // namespace snapcast
}  // namespace esphome
#endif
