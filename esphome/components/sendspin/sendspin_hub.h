#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/types.h>

#ifdef USE_SENDSPIN_ARTWORK
#include <sendspin/artwork_role.h>
#endif
#ifdef USE_SENDSPIN_CONTROLLER
#include <sendspin/controller_role.h>
#endif
#ifdef USE_SENDSPIN_METADATA
#include <sendspin/metadata_role.h>
#endif
#ifdef USE_SENDSPIN_PLAYER
#include <sendspin/player_role.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace esphome::sendspin_ {

/// @brief Setup priorities for the sendspin hub and its child components.
///
/// Centralized here so every sendspin component orders itself relative to the hub
/// without each subcomponent having to pick a priority independently. Children run
/// one step later than hub so they can assume hub's setup() has already completed.
namespace sendspin_priority {
// AFTER_WIFI so the hub runs after the wifi/ethernet drivers are up and we can read the active
// interface's MAC for client_id.
inline constexpr float HUB = esphome::setup_priority::AFTER_WIFI;
inline constexpr float CHILD = HUB - 1.0f;
}  // namespace sendspin_priority

// ---------------------------------------------------------------------------
// Persistent storage (ESPPreferences POD blobs, one NVS key per library key).
//
// The library's SendspinPersistenceProvider is a plain byte store keyed by the
// sendspin::persistence_keys constants; the library owns all serialization.
// ESPPreferences stores fixed-size trivially-copyable structs, so each key gets
// a fixed-capacity staging struct with an explicit length prefix. A stored
// length of zero is the "erased" sentinel written by erase_blob(): it is
// distinct from a never-written key, which is how the YAML initial_* seeds know
// they still apply (they seed only while a key has never been written).
// ---------------------------------------------------------------------------

/// @brief Fixed-capacity staging struct for one persistence blob.
template<size_t CAP> struct SendspinBlobPref {
  uint16_t len;
  uint8_t data[CAP];
};

/// @brief Raw 32-byte X25519 private key (persistence_keys::KEYPAIR).
inline constexpr size_t SENDSPIN_KEYPAIR_BLOB_CAP = 32;
/// @brief Codec blob holding the whole pairing-record array (persistence_keys::RECORDS).
/// Sized for SENDSPIN_MAX_PAIRING_RECORDS plus the one extra record a pairing supersede
/// transiently persists, at roughly 250 encoded bytes per record with label headroom (a
/// measured label-free record encodes to ~180 bytes).
///
/// Every write persists the full CAP+2 bytes even when the encoded content is smaller:
/// ESPPreferenceObject's load requires the exact stored size, so a variable-length write
/// would need a size-discovery primitive the preferences API does not have (a separate
/// length key cannot be updated atomically with the data and a torn write would read as
/// "absent", losing every record). Records mutate only on pairing/management events, so
/// the write amplification is bounded by design, not by frequency.
inline constexpr size_t SENDSPIN_RECORDS_BLOB_CAP = 2400;
/// @brief Codec blob holding the accepted Pairing PSK (persistence_keys::PAIRING_PSK).
inline constexpr size_t SENDSPIN_PAIRING_PSK_BLOB_CAP = 240;
/// @brief Raw UTF-8 static PIN string (persistence_keys::STATIC_PIN).
inline constexpr size_t SENDSPIN_STATIC_PIN_BLOB_CAP = 16;
/// @brief Codec blob holding the pairing policy config (persistence_keys::PAIR_CONFIG).
inline constexpr size_t SENDSPIN_PAIR_CONFIG_BLOB_CAP = 288;
/// @brief Raw UTF-8 base64url server_id (persistence_keys::LAST_PLAYED).
inline constexpr size_t SENDSPIN_LAST_PLAYED_BLOB_CAP = 48;
/// @brief ASCII decimal delay in milliseconds (persistence_keys::STATIC_DELAY).
inline constexpr size_t SENDSPIN_STATIC_DELAY_BLOB_CAP = 8;

/// @brief Cap on long-term pairing records, passed to the library as max_pairing_records.
/// Keeps the encoded RECORDS blob inside SENDSPIN_RECORDS_BLOB_CAP.
inline constexpr size_t SENDSPIN_MAX_PAIRING_RECORDS = 8;

#ifdef USE_SENDSPIN_PLAYER
/// @brief Pre-encryption storage layout for the player static delay (raw uint16 under the
/// same NVS key the ASCII-decimal STATIC_DELAY blob now uses). Kept only so an upgraded
/// device's tuned delay survives until the first new-format save overwrites it.
struct LegacyStaticDelayPref {
  uint16_t delay_ms;
};
#endif

/// @brief Thin adapter over sendspin::SendspinClient.
///
/// The hub owns a SendspinClient instance and bridges its listener/provider interfaces to ESPHome's CallbackManager for
/// fan-out to child components.
///  - Provides persistence via ESPPreferenceObject and WiFi power management integration.
///  - Handles Sendspin roles that apply to multiple child components (artwork, controller, metadata) so their events
///    can be fanned out. Roles specific to a single component (player) are configured by the hub but owned by the
///    child thereafter, since no fan-out is needed.
///
/// The sendspin-cpp library follows this design:
///  - Core and role configuration are passed at client/role construction time as structs. Built in our `setup()`.
///  - Library -> user code communication happens via two interface types the user implements and registers in our
///    `setup()`: listener interfaces (for events the library pushes; e.g., group updates) and provider interfaces
///    (for services the library pulls; e.g., persistence, network readiness).
///  - User -> library communication uses exposed functions on the client and role objects that the user calls.
class SendspinHub final : public Component,
#ifdef USE_SENDSPIN_ARTWORK
                          public sendspin::ArtworkRoleListener,
#endif
#ifdef USE_SENDSPIN_CONTROLLER
                          public sendspin::ControllerRoleListener,
#endif
#ifdef USE_SENDSPIN_METADATA
                          public sendspin::MetadataRoleListener,
#endif
                          public sendspin::SendspinClientListener,
                          public sendspin::SendspinNetworkProvider,
                          public sendspin::SendspinPersistenceProvider {
 public:
  float get_setup_priority() const override { return sendspin_priority::HUB; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// @brief Connects the underlying client to the given Sendspin server.
  ///
  /// No-op if the hub's client is not ready (e.g. setup() has not completed).
  /// Must be called from the main loop thread.
  /// @param url WebSocket URL of the Sendspin server, starting with `ws://` (e.g. `ws://host:port/path`).
  void connect_to_server(const std::string &url);

  /// @brief Disconnects the underlying client from the current server.
  ///
  /// Sends a `client/goodbye` message with the given reason before closing the connection.
  /// No-op if the hub's client is not ready. Must be called from the main loop thread.
  /// @param reason Reason reported to the server:
  ///   - `ANOTHER_SERVER`: client is switching to another server.
  ///   - `SHUTDOWN`: client is shutting down.
  ///   - `RESTART`: client is restarting.
  ///   - `USER_REQUEST`: user explicitly requested disconnect.
  void disconnect_from_server(sendspin::SendspinGoodbyeReason reason);

  /// @brief Updates the client's reported playback state on the server.
  ///
  /// No-op if the hub's client is not ready. Must be called from the main loop thread.
  /// @param state New client state:
  ///   - `SYNCHRONIZED`: client is synchronized and playing from the server.
  ///   - `ERROR`: client encountered a playback error.
  ///   - `EXTERNAL_SOURCE`: client is playing from a non-Sendspin source.
  void update_state(sendspin::SendspinClientState state);

  /// @brief Signals that the operator performed the device pairing-window gesture.
  ///
  /// Forwards to SendspinClient::confirm_pairing_window(); advances a pending gesture-gated
  /// PIN pairing. No-op if the hub's client is not ready.
  void confirm_pairing_window();

  // --- Configuration setters (called from codegen) ---

  template<typename F> void add_group_update_callback(F &&callback) {
    this->group_update_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_open_pairing_window_callback(F &&callback) {
    this->open_pairing_window_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_close_pairing_window_callback(F &&callback) {
    this->close_pairing_window_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_display_pairing_pin_callback(F &&callback) {
    this->display_pairing_pin_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_clear_pairing_pin_callback(F &&callback) {
    this->clear_pairing_pin_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_pairing_succeeded_callback(F &&callback) {
    this->pairing_succeeded_callbacks_.add(std::forward<F>(callback));
  }

  template<typename F> void add_on_pairing_failed_callback(F &&callback) {
    this->pairing_failed_callbacks_.add(std::forward<F>(callback));
  }

  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  /// @brief Sets the initial static PIN from YAML (exactly 8 decimal digits).
  ///
  /// Seeds the library's STATIC_PIN blob while that key has never been written, seeds
  /// static_pin_enabled into the first-boot pairing config, and makes build_client_config_()
  /// advertise pairing-window support. Once a server persists a PIN change or clear via
  /// management/set-pairing-config, the stored value wins over this seed.
  void set_initial_static_pin(const std::string &pin) { this->initial_static_pin_ = pin; }

  /// @brief Sets the initial accepted Pairing PSK from YAML.
  ///
  /// Seeds the library's PAIRING_PSK blob while that key has never been written; once the
  /// library or a server writes or clears the accepted PSK, the stored value wins. psk_id is
  /// derived from the secret at codegen time (base64url(SHA-256("sendspin-psk-id-v1" || psk))).
  void set_initial_pairing_psk(const std::string &psk_id, const std::array<uint8_t, 32> &psk) {
    sendspin::SendspinPairingPsk value;
    value.psk_id = psk_id;
    value.psk = psk;
    this->initial_pairing_psk_ = std::move(value);
  }

  /// @brief Sets the first-boot default for unpaired (Sentinel) access.
  ///
  /// Passed through to SendspinClientConfig::initial_unpaired_access_enabled and folded into
  /// the first-boot pairing config seed; a server's management/set-pairing-config decision
  /// wins once any pairing config has been persisted.
  void set_initial_unpaired_access_enabled(bool enabled) { this->initial_unpaired_access_enabled_ = enabled; }

  /// @brief Marks that the YAML config can display a dynamic pairing PIN to the user.
  ///
  /// Set from codegen when an on_display_pairing_pin automation is configured. Makes
  /// build_client_config_() advertise PIN-display support so the library offers the
  /// dynamic_pin pair method to servers.
  void set_pin_display_supported(bool supported) { this->pin_display_supported_ = supported; }

  /// @brief Marks that the YAML config implements the operator pairing-window gesture UI.
  ///
  /// Set from codegen when an on_open_pairing_window automation is configured. A configured
  /// initial static PIN implies pairing-window support even without the automation (the
  /// sendspin.confirm_pairing_window action alone can confirm the gesture).
  void set_pairing_window_supported(bool supported) { this->pairing_window_supported_ = supported; }

  // --- Sendspin role specific methods ---

#ifdef USE_SENDSPIN_ARTWORK
  void set_artwork_config(const sendspin::ArtworkRoleConfig &config) { this->artwork_config_ = config; }

  /// @brief Acknowledges the most recent artwork delivery (display or clear) for a slot.
  ///
  /// Every slot is configured with the library's require_frame_done gate, which withholds the
  /// next delivery for the slot until this is called. Exactly one ack is owed per delivery; a
  /// redundant call is a safe no-op in the library. Must be called from the main loop thread.
  void artwork_frame_done(uint8_t slot);

  template<typename F> void add_image_decode_callback(F &&callback) {
    this->artwork_image_decode_callbacks_.add(std::forward<F>(callback));
  }
  template<typename F> void add_image_display_callback(F &&callback) {
    this->artwork_image_display_callbacks_.add(std::forward<F>(callback));
  }
  template<typename F> void add_image_clear_callback(F &&callback) {
    this->artwork_image_clear_callbacks_.add(std::forward<F>(callback));
  }
#endif

#ifdef USE_SENDSPIN_CONTROLLER
  void send_client_command(sendspin::SendspinControllerCommand command, std::optional<uint8_t> volume = std::nullopt,
                           std::optional<bool> mute = std::nullopt);

  template<typename F> void add_controller_state_callback(F &&callback) {
    this->controller_state_callbacks_.add(std::forward<F>(callback));
  }

  /// @brief Registers a callback that fires when the connection is lost and the cached controller state is dropped.
  template<typename F> void add_controller_state_clear_callback(F &&callback) {
    this->controller_state_clear_callbacks_.add(std::forward<F>(callback));
  }
#endif

#ifdef USE_SENDSPIN_METADATA
  /// @brief Registers a callback that fires when the server sends metadata.
  ///
  /// Also fires when the connection is lost, with an all-empty state object (every field nullopt, timestamp 0) meaning
  /// the cached metadata was dropped. Subscribers must treat an absent field as cleared, not as no update.
  template<typename F> void add_metadata_update_callback(F &&callback) {
    this->metadata_update_callbacks_.add(std::forward<F>(callback));
  }

  /// @brief Returns the interpolated track progress in milliseconds, or 0 if the hub is not yet ready.
  uint32_t get_track_progress_ms() const;
#endif

#ifdef USE_SENDSPIN_PLAYER
  void set_listener(sendspin::PlayerRoleListener *listener) { this->player_listener_ = listener; }
  void set_player_config(const sendspin::PlayerRoleConfig &config) { this->player_config_ = config; }

  /// @brief Child components call this to get the PlayerRole instance after setup, so they can push updates to it.
  sendspin::PlayerRole *get_player_role();
#endif

 protected:
  /// @brief Builds the SendspinClientConfig from ESPHome configuration and platform info.
  sendspin::SendspinClientConfig build_client_config_();

  /// @brief Writes the active network interface's MAC into @p buf and returns its data pointer.
  /// Uses the ethernet MAC if ethernet is configured, otherwise the base MAC (used by wifi).
  static const char *get_client_id_into_buffer(std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf);

  // --- SendspinClientListener overrides ---
  void on_group_update(const sendspin::GroupUpdateObject &group) override;

  void on_request_high_performance() override;

  void on_release_high_performance() override;

  void on_open_pairing_window() override;

  void on_close_pairing_window() override;

  void on_display_pairing_pin(const std::string &pin) override;

  void on_clear_pairing_pin() override;

  void on_pairing_succeeded(const std::string &server_id) override;

  void on_pairing_failed(const std::string &server_id, sendspin::SendspinPairAbortReason reason) override;

  // --- SendspinNetworkProvider override ---
  bool is_network_ready() override;

  // --- SendspinPersistenceProvider overrides ---
  // Plain byte store keyed by sendspin::persistence_keys; the library owns all
  // serialization. Pairing material (keypair, records, pairing_psk, static_pin, pair_config,
  // and every erase) is flushed to flash immediately rather than waiting out the preference
  // syncer's write interval, which would let a power cut strand the device on a key its
  // server no longer accepts; last_played and static_delay ride the normal batching.
  //
  // ESPHome's preference queue is main-loop-only, and the library may call
  // save_blob(RECORDS) from its network thread during pairing finalize. That one write is
  // therefore staged under pending_records_mutex_ and persisted from loop() instead.
  std::optional<std::vector<uint8_t>> load_blob(const std::string &key) override;
  bool save_blob(const std::string &key, const uint8_t *data, size_t len) override;
  bool erase_blob(const std::string &key) override;

  /// @brief Returns the fabricated first-boot PAIR_CONFIG seed blob, or nullopt.
  ///
  /// Only called when the PAIR_CONFIG key has never been written. Folds the YAML initial_*
  /// values (static PIN enabling, unpaired access) into the config the library loads on its
  /// genuine first boot; the library persists the real config during first-boot
  /// provisioning, so this fabrication happens at most once per device lifetime.
  std::optional<std::vector<uint8_t>> first_boot_pairing_config_seed_();

  // --- Sendspin role specific methods/overrides/member variables ---

#ifdef USE_SENDSPIN_ARTWORK
  void on_image_decode(uint8_t slot, const uint8_t *data, size_t length, sendspin::SendspinImageFormat format) override;

  void on_image_display(uint8_t slot, uint32_t lateness_ms) override;

  void on_image_clear(uint8_t slot) override;

  sendspin::ArtworkRoleConfig artwork_config_{};
  sendspin::ArtworkRole *artwork_role_{nullptr};

  // Callback fan-out to child components; they filter by slot as needed.
  CallbackManager<void(uint8_t, const uint8_t *, size_t, sendspin::SendspinImageFormat)>
      artwork_image_decode_callbacks_{};
  CallbackManager<void(uint8_t, uint32_t)> artwork_image_display_callbacks_{};
  CallbackManager<void(uint8_t)> artwork_image_clear_callbacks_{};
#endif

#ifdef USE_SENDSPIN_CONTROLLER
  sendspin::ControllerRole *controller_role_{nullptr};

  void on_controller_state(const sendspin::ServerStateControllerObject &state) override;

  void on_controller_state_clear() override;

  // Callback fan-out to child components; they filter as needed. Only a media_player subscribes, while the switch
  // action and the media source enable the controller role without one, so keep the idle cost to a single pointer.
  LazyCallbackManager<void(const sendspin::ServerStateControllerObject &)> controller_state_callbacks_{};
  LazyCallbackManager<void()> controller_state_clear_callbacks_{};
#endif

#ifdef USE_SENDSPIN_METADATA
  sendspin::MetadataRole *metadata_role_{nullptr};

  void on_metadata(const sendspin::ServerMetadataStateObject &metadata) override;

  void on_metadata_clear() override;

  // Callback fan-out to child components; they filter as needed
  CallbackManager<void(const sendspin::ServerMetadataStateObject &)> metadata_update_callbacks_{};
#endif

#ifdef USE_SENDSPIN_PLAYER
  sendspin::PlayerRoleListener *player_listener_{nullptr};
  sendspin::PlayerRoleConfig player_config_{};
#endif

  // --- Core member variables ---

  // Persistence (ESPPreferences, one NVS key per persistence_keys constant), initialized
  // in setup(). legacy_static_delay_pref_ reads the pre-encryption uint16 layout stored
  // under the same NVS key as static_delay_pref_'s ASCII-decimal blob.
  ESPPreferenceObject keypair_pref_;
  ESPPreferenceObject records_pref_;
  ESPPreferenceObject pairing_psk_pref_;
  ESPPreferenceObject static_pin_pref_;
  ESPPreferenceObject pair_config_pref_;
  ESPPreferenceObject last_played_pref_;
  ESPPreferenceObject static_delay_pref_;
#ifdef USE_SENDSPIN_PLAYER
  ESPPreferenceObject legacy_static_delay_pref_;
#endif

  // Records blob staged by save_blob() when it runs on the library's network thread, drained
  // and persisted by loop(). Latest-wins: the blob is the whole record array, so a newer
  // write supersedes an undrained older one outright.
  std::mutex pending_records_mutex_;
  std::vector<uint8_t> pending_records_;
  bool pending_records_valid_{false};

  std::unique_ptr<sendspin::SendspinClient> client_;

  // Callback fan-out to child components
  CallbackManager<void(const sendspin::GroupUpdateObject &)> group_update_callbacks_{};

  // Pairing-window gesture fan-out to automation triggers.
  CallbackManager<void()> open_pairing_window_callbacks_{};
  CallbackManager<void()> close_pairing_window_callbacks_{};

  // Dynamic-PIN display fan-out to automation triggers.
  CallbackManager<void(std::string)> display_pairing_pin_callbacks_{};
  CallbackManager<void()> clear_pairing_pin_callbacks_{};

  // Pairing outcome fan-out to automation triggers. Failed carries (server_id, reason string).
  CallbackManager<void(std::string)> pairing_succeeded_callbacks_{};
  CallbackManager<void(std::string, std::string)> pairing_failed_callbacks_{};

  // Initial static PIN from YAML (absent = not configured). Seeds the STATIC_PIN blob and
  // the first-boot pairing config only until those keys are first written.
  std::optional<std::string> initial_static_pin_{};

  // Initial accepted Pairing PSK from YAML (absent = not configured). Seeds the
  // PAIRING_PSK blob only until that key is first written.
  std::optional<sendspin::SendspinPairingPsk> initial_pairing_psk_{};

  // First-boot default for unpaired (Sentinel) access, from YAML.
  bool initial_unpaired_access_enabled_{false};

  // True when the YAML config has an on_display_pairing_pin automation, enabling
  // dynamic-PIN pairing (see set_pin_display_supported).
  bool pin_display_supported_{false};

  // True when the YAML config has an on_open_pairing_window automation (see
  // set_pairing_window_supported); a configured initial static PIN also implies support.
  bool pairing_window_supported_{false};

  bool task_stack_in_psram_{false};
};

/// @brief Base class for all sendspin subcomponents.
///
/// Consolidates the Component + Parented<SendspinHub> inheritance and pins the setup
/// priority so the hub's setup() always runs before any child. Subcomponents should
/// inherit from this instead of listing Component/Parented individually and must not
/// override get_setup_priority().
class SendspinChild : public Component, public Parented<SendspinHub> {
 public:
  float get_setup_priority() const override { return sendspin_priority::CHILD; }
};

/// @brief Base class for sendspin subcomponents that need polling behavior.
///
/// Same purpose as SendspinChild but inherits from PollingComponent for subcomponents
/// that poll on a fixed interval. Subcomponents should inherit from this instead of
/// listing PollingComponent/Parented individually and must not override get_setup_priority().
class SendspinPollingChild : public PollingComponent, public Parented<SendspinHub> {
 public:
  float get_setup_priority() const override { return sendspin_priority::CHILD; }
};

}  // namespace esphome::sendspin_

#endif  // USE_ESP32
