#include "sendspin_hub.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include <sendspin/persistence_codec.h>

#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace esphome::sendspin_ {

static const char *const TAG = "sendspin.hub";

namespace {

/// @brief Wire-style string for a pairing abort reason, exposed to on_pairing_failed automations.
const char *pair_abort_reason_to_string(sendspin::SendspinPairAbortReason reason) {
  using sendspin::SendspinPairAbortReason;
  switch (reason) {
    case SendspinPairAbortReason::ATTEMPT_TIMEOUT:
      return "attempt_timeout";
    case SendspinPairAbortReason::CONCURRENT_ATTEMPT:
      return "concurrent_attempt";
    case SendspinPairAbortReason::METHOD_NOT_SUPPORTED:
      return "method_not_supported";
    case SendspinPairAbortReason::PIN_LENGTH_UNACCEPTABLE:
      return "pin_length_unacceptable";
    case SendspinPairAbortReason::PIN_MISMATCH:
      return "pin_mismatch";
    case SendspinPairAbortReason::USER_CANCELLED:
      return "user_cancelled";
    case SendspinPairAbortReason::UNKNOWN:
      break;
  }
  return "unknown";
}

/// @brief What a stored blob preference holds. ABSENT means the NVS key has never been
/// written (the YAML initial_* seeds apply only in this state); ERASED is the zero-length
/// sentinel erase_blob() writes, meaning a server explicitly cleared the value.
enum class BlobState : uint8_t { ABSENT, ERASED, PRESENT };

/// @brief Overwrites a buffer with zeroes through a volatile pointer, so the write survives
/// dead-store elimination on memory that is about to be freed (a plain memset on a buffer
/// that is never read again is legal for the compiler to delete). Mirrors the sendspin-cpp
/// library's secure_zero(), which lives in a private library header and is not reachable
/// from here. The staging buffers below hold the same secrets the library wipes on its own
/// side of the provider interface (the X25519 private key, pairing-record PSKs, the Pairing
/// PSK), so the copies made here must not outlive their use either.
void secure_wipe(void *data, size_t len) {
  volatile auto *p = static_cast<volatile uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    p[i] = 0;
  }
}

/// @brief Loads a SendspinBlobPref<CAP> preference. When `out` is non-null, fills it with
/// the stored bytes on PRESENT. Staging goes through the heap: the records blob is multiple
/// KB and load_blob(RECORDS) runs on task stacks sized for the Noise handshake, not for it.
/// The staging buffer is wiped before it is freed.
template<size_t CAP> BlobState read_blob(ESPPreferenceObject &pref, const char *key, std::vector<uint8_t> *out) {
  auto buf = std::make_unique<SendspinBlobPref<CAP>>();
  if (!pref.load(buf.get())) {
    return BlobState::ABSENT;
  }
  BlobState state = BlobState::PRESENT;
  if (buf->len == 0) {
    state = BlobState::ERASED;
  } else if (buf->len > CAP) {
    ESP_LOGW(TAG, "Stored \"%s\" blob length %u exceeds capacity %zu; ignoring it", key, buf->len, CAP);
    state = BlobState::ERASED;
  } else if (out != nullptr) {
    out->assign(buf->data, buf->data + buf->len);
  }
  secure_wipe(buf.get(), sizeof(*buf));
  return state;
}

/// @brief Saves bytes into a SendspinBlobPref<CAP> preference. Main loop only: ESPHome's
/// preference queue is not synchronized, so every caller must already be on the main loop
/// (see SendspinHub::save_blob for how the one off-loop write is deferred there).
/// The unused tail is zeroed so identical values produce identical stored bytes and the
/// NVS layer's changed-data check can skip redundant writes; the whole staging buffer is
/// wiped again before it is freed.
template<size_t CAP> bool write_blob(ESPPreferenceObject &pref, const char *key, const uint8_t *data, size_t len) {
  if (len > CAP) {
    ESP_LOGW(TAG, "\"%s\" blob of %zu bytes exceeds capacity %zu; rejecting write", key, len, CAP);
    return false;
  }
  auto buf = std::make_unique<SendspinBlobPref<CAP>>();
  std::memset(buf.get(), 0, sizeof(*buf));
  buf->len = static_cast<uint16_t>(len);
  if (len > 0) {
    std::memcpy(buf->data, data, len);
  }
  bool ok = pref.save(buf.get());
  if (!ok) {
    ESP_LOGW(TAG, "Failed to persist \"%s\" blob (%zu bytes)", key, len);
  }
  secure_wipe(buf.get(), sizeof(*buf));
  return ok;
}

/// @brief Flushes queued preference writes to flash immediately. Pairing material must not
/// wait out the preference syncer's write interval (60 s by default): a power cut inside that
/// window would strand the device holding a key its server no longer accepts. Main loop only.
void flush_preferences(const char *key) {
  // sync() reports failure for the whole batch, including other components' writes, so its
  // result is not a per-key durability answer -- log it and move on.
  if (!global_preferences->sync()) {
    ESP_LOGW(TAG, "Preference sync reported a failure while persisting \"%s\"", key);
  }
}

}  // namespace

#ifdef USE_SENDSPIN_ARTWORK
// Indexed by the library enums, which start at zero and are contiguous.
static const char *const IMAGE_SOURCE_NAMES[] = {"ALBUM", "ARTIST", "NONE"};
static const char *const IMAGE_FORMAT_NAMES[] = {"JPEG", "PNG", "BMP"};
#endif

void SendspinHub::setup() {
  auto config = this->build_client_config_();
  this->client_ = std::make_unique<sendspin::SendspinClient>(std::move(config));

  // Set up persistence (preferences must be initialized before providers are added to the
  // client). One NVS key per persistence_keys constant; see the SendspinBlobPref comment in
  // sendspin_hub.h for the storage layout.
  this->keypair_pref_ =
      global_preferences->make_preference<SendspinBlobPref<SENDSPIN_KEYPAIR_BLOB_CAP>>(fnv1a_hash("sendspin_keypair"));
  this->records_pref_ =
      global_preferences->make_preference<SendspinBlobPref<SENDSPIN_RECORDS_BLOB_CAP>>(fnv1a_hash("sendspin_records"));
  this->pairing_psk_pref_ = global_preferences->make_preference<SendspinBlobPref<SENDSPIN_PAIRING_PSK_BLOB_CAP>>(
      fnv1a_hash("sendspin_pair_psk"));
  this->static_pin_pref_ = global_preferences->make_preference<SendspinBlobPref<SENDSPIN_STATIC_PIN_BLOB_CAP>>(
      fnv1a_hash("sendspin_static_pin"));
  this->pair_config_pref_ = global_preferences->make_preference<SendspinBlobPref<SENDSPIN_PAIR_CONFIG_BLOB_CAP>>(
      fnv1a_hash("sendspin_pair_cfg"));
  this->last_played_pref_ = global_preferences->make_preference<SendspinBlobPref<SENDSPIN_LAST_PLAYED_BLOB_CAP>>(
      fnv1a_hash("sendspin_last_played"));
  this->static_delay_pref_ = global_preferences->make_preference<SendspinBlobPref<SENDSPIN_STATIC_DELAY_BLOB_CAP>>(
      fnv1a_hash("sendspin_static_delay"));
#ifdef USE_SENDSPIN_PLAYER
  // Pre-encryption uint16 layout under the same NVS key; read-only migration source.
  this->legacy_static_delay_pref_ =
      global_preferences->make_preference<LegacyStaticDelayPref>(fnv1a_hash("sendspin_static_delay"));
#endif

  // Wire providers and client listener
  this->client_->set_listener(this);
  this->client_->set_network_provider(this);
  this->client_->set_persistence_provider(this);

#ifdef USE_SENDSPIN_ARTWORK
  this->artwork_role_ = &this->client_->add_artwork(this->artwork_config_);
  this->artwork_role_->set_listener(this);
#endif

#ifdef USE_SENDSPIN_CONTROLLER
  this->controller_role_ = &this->client_->add_controller();
  this->controller_role_->set_listener(this);
#endif

#ifdef USE_SENDSPIN_METADATA
  this->metadata_role_ = &this->client_->add_metadata();
  this->metadata_role_->set_listener(this);
#endif

#ifdef USE_SENDSPIN_PLAYER
  this->client_->add_player(this->player_config_).set_listener(this->player_listener_);
#endif

  if (!this->client_->start_server()) {
    ESP_LOGE(TAG, "Failed to start Sendspin server");
    this->mark_failed();
    return;
  }
}

void SendspinHub::loop() {
  this->client_->loop();

  // Persist a records blob staged by save_blob() from the library's network thread.
  {
    std::vector<uint8_t> records;
    bool have_records = false;
    {
      std::lock_guard<std::mutex> lock(this->pending_records_mutex_);
      if (this->pending_records_valid_) {
        records.swap(this->pending_records_);
        this->pending_records_valid_ = false;
        have_records = true;
      }
    }
    if (have_records) {
      write_blob<SENDSPIN_RECORDS_BLOB_CAP>(this->records_pref_, sendspin::persistence_keys::RECORDS, records.data(),
                                            records.size());
      flush_preferences(sendspin::persistence_keys::RECORDS);
      secure_wipe(records.data(), records.size());
    }
  }

  if (this->pairing_token_dirty_) {
    this->pairing_token_dirty_ = false;
    std::string token = this->client_->pairing_token().value_or("");
    if (token != this->last_pairing_token_) {
      this->last_pairing_token_ = token;
      this->pairing_token_callbacks_.call(std::move(token));
    }
  }
}

void SendspinHub::dump_config() {
  // client_id is derived from the static keypair, so it exists only once start_server()
  // has run (setup() succeeded).
  const char *client_id = "(unavailable)";
  if (this->client_ != nullptr && !this->client_->client_id().empty()) {
    client_id = this->client_->client_id().c_str();
  }
  char mac_buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  ESP_LOGCONFIG(TAG,
                "Sendspin Hub:\n"
                "  Client ID: %s\n"
                "  MAC address: %s\n"
                "  Task stack in PSRAM: %s\n"
                "  Initial static PIN: %s\n"
                "  Initial pairing PSK: %s\n"
                "  Initial unpaired access: %s\n"
                "  Dynamic PIN display: %s\n"
                "  Pairing window gesture: %s",
                client_id, get_client_id_into_buffer(mac_buf), YESNO(this->task_stack_in_psram_),
                this->initial_static_pin_.has_value() ? "configured" : "not configured",
                this->initial_pairing_psk_.has_value() ? "configured" : "not configured",
                YESNO(this->initial_unpaired_access_enabled_), YESNO(this->pin_display_supported_),
                YESNO(this->pairing_window_supported_ || this->initial_static_pin_.has_value()));

#ifdef USE_SENDSPIN_ARTWORK
  // Slot indices come from the order the image platform entries were declared, so the log is the
  // only place the mapping from a slot to the artwork it asked for can be read back.
  uint8_t slot = 0;
  for (const auto &preference : this->artwork_config_.preferred_formats) {
    ESP_LOGCONFIG(TAG, "  Artwork slot %u: %s as %s, %ux%u, display offset %" PRId32 " ms", slot++,
                  IMAGE_SOURCE_NAMES[static_cast<uint8_t>(preference.source)],
                  IMAGE_FORMAT_NAMES[static_cast<uint8_t>(preference.format)], preference.width, preference.height,
                  preference.display_offset_ms);
  }
#endif
}

// --- Delegating methods ---

// THREAD CONTEXT: Main loop (invoked from Sendspin components)
void SendspinHub::connect_to_server(const std::string &url) {
  if (this->is_ready()) {
    this->client_->connect_to(url);
  }
}

// THREAD CONTEXT: Main loop (invoked from Sendspin components)
void SendspinHub::disconnect_from_server(sendspin::SendspinGoodbyeReason reason) {
  if (this->is_ready()) {
    this->client_->disconnect(reason);
  }
}

// THREAD CONTEXT: Main loop (invoked from Sendspin components)
void SendspinHub::update_state(sendspin::SendspinClientState state) {
  if (this->is_ready()) {
    this->client_->update_state(state);
  }
}

// THREAD CONTEXT: Main loop (invoked from the sendspin.confirm_pairing_window action)
void SendspinHub::confirm_pairing_window() {
  if (this->is_ready()) {
    this->client_->confirm_pairing_window();
  }
}

const char *SendspinHub::get_client_id_into_buffer(std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf) {
  // The server matches client_id against the L2 source MAC of the device's multicast traffic.
  // ESP-IDF derives the ethernet MAC as base+3 by default on ESP32-S3, so we cannot use the
  // eFuse base MAC when ethernet is the active interface.
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr) {
    return ethernet::global_eth_component->get_eth_mac_address_pretty_into_buffer(buf);
  }
#endif
  return get_mac_address_pretty_into_buffer(buf);
}

sendspin::SendspinClientConfig SendspinHub::build_client_config_() {
  sendspin::SendspinClientConfig config;

  // client_id is derived by the library from the static keypair; the MAC is reported in
  // device_info so servers can match the client against its multicast traffic.
  char mac_buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  config.mac_address = SendspinHub::get_client_id_into_buffer(mac_buf);
  config.name = App.get_friendly_name();
  config.product_name = App.get_name();
  config.manufacturer = "ESPHome";
  config.software_version = ESPHOME_VERSION;
  config.httpd_psram_stack = this->task_stack_in_psram_;

  // An on_display_pairing_pin automation means the device can show a dynamic PIN
  // (see the on_display_pairing_pin / on_clear_pairing_pin triggers).
  config.pin_display_supported = this->pin_display_supported_;

  // An on_open_pairing_window automation means the operator can be prompted for the
  // pairing-window gesture; a YAML static PIN implies the same (the gesture can be
  // confirmed through the sendspin.confirm_pairing_window action alone).
  config.pairing_window_supported = this->pairing_window_supported_ || this->initial_static_pin_.has_value();

  // First-boot default for unpaired (Sentinel) access; the library seeds and persists it
  // only on a genuine first boot (see first_boot_pairing_config_seed_(), which folds the
  // same value into the fabricated config on the paths where that seed loads instead).
  config.initial_unpaired_access_enabled = this->initial_unpaired_access_enabled_;

  // Keep the encoded RECORDS blob inside SENDSPIN_RECORDS_BLOB_CAP.
  config.max_pairing_records = SENDSPIN_MAX_PAIRING_RECORDS;

  return config;
}

// --- SendspinClientListener overrides ---
// THREAD CONTEXT: Main loop (fired from client_->loop())

void SendspinHub::on_group_update(const sendspin::GroupUpdateObject &group) {
  this->group_update_callbacks_.call(group);
}

void SendspinHub::on_request_high_performance() {
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->request_high_performance();
    wifi::global_wifi_component->request_roaming_suppression();
  }
#endif
}

void SendspinHub::on_release_high_performance() {
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->release_high_performance();
    wifi::global_wifi_component->release_roaming_suppression();
  }
#endif
}

void SendspinHub::on_open_pairing_window() { this->open_pairing_window_callbacks_.call(); }

void SendspinHub::on_close_pairing_window() { this->close_pairing_window_callbacks_.call(); }

void SendspinHub::on_display_pairing_pin(const std::string &pin) { this->display_pairing_pin_callbacks_.call(pin); }

void SendspinHub::on_clear_pairing_pin() { this->clear_pairing_pin_callbacks_.call(); }

void SendspinHub::on_pairing_succeeded(const std::string &server_id) {
  this->pairing_succeeded_callbacks_.call(server_id);
}

void SendspinHub::on_pairing_failed(const std::string &server_id, sendspin::SendspinPairAbortReason reason) {
  this->pairing_failed_callbacks_.call(server_id, pair_abort_reason_to_string(reason));
}

// --- SendspinNetworkProvider override ---

// THREAD CONTEXT: Main loop (polled by client_->loop())
bool SendspinHub::is_network_ready() { return network::is_connected(); }

// --- SendspinPersistenceProvider overrides ---
// See the interface comment in sendspin_hub.h: pure byte store, immediate flush for pairing
// material, batched writes for last_played/static_delay, zero-length sentinel for erases.
// Everything runs on the main loop except save_blob(RECORDS), which may also fire on the
// library's network thread during pairing finalize and is staged for loop() to persist.

std::optional<std::vector<uint8_t>> SendspinHub::load_blob(const std::string &key) {
  namespace keys = sendspin::persistence_keys;
  std::vector<uint8_t> out;

  if (key == keys::KEYPAIR) {
    if (read_blob<SENDSPIN_KEYPAIR_BLOB_CAP>(this->keypair_pref_, keys::KEYPAIR, &out) == BlobState::PRESENT) {
      return out;
    }
    return std::nullopt;
  }

  if (key == keys::RECORDS) {
    // A staged write from the network thread has not reached the preference layer yet, so
    // answer from it rather than from the older stored bytes.
    {
      std::lock_guard<std::mutex> lock(this->pending_records_mutex_);
      if (this->pending_records_valid_) {
        return this->pending_records_;
      }
    }
    if (read_blob<SENDSPIN_RECORDS_BLOB_CAP>(this->records_pref_, keys::RECORDS, &out) == BlobState::PRESENT) {
      return out;
    }
    return std::nullopt;
  }

  if (key == keys::PAIRING_PSK) {
    BlobState state = read_blob<SENDSPIN_PAIRING_PSK_BLOB_CAP>(this->pairing_psk_pref_, keys::PAIRING_PSK, &out);
    if (state == BlobState::PRESENT) {
      return out;
    }
    // The YAML initial Pairing PSK applies only while the key has never been written: the
    // library's own first-boot provisioning and any server-driven change or clear all write
    // it, and the stored state (including the erased sentinel) wins from then on.
    if (state == BlobState::ABSENT && this->initial_pairing_psk_.has_value()) {
      std::string blob = sendspin::encode_pairing_psk(this->initial_pairing_psk_.value());
      std::vector<uint8_t> encoded(blob.begin(), blob.end());
      // The encoded text embeds the PSK; the library wipes the returned vector after
      // decoding it, but this local copy is ours to wipe.
      secure_wipe(blob.data(), blob.size());
      return encoded;
    }
    return std::nullopt;
  }

  if (key == keys::STATIC_PIN) {
    BlobState state = read_blob<SENDSPIN_STATIC_PIN_BLOB_CAP>(this->static_pin_pref_, keys::STATIC_PIN, &out);
    if (state == BlobState::PRESENT) {
      return out;
    }
    // Same never-written seed rule as the Pairing PSK above.
    if (state == BlobState::ABSENT && this->initial_static_pin_.has_value()) {
      const std::string &pin = this->initial_static_pin_.value();
      return std::vector<uint8_t>(pin.begin(), pin.end());
    }
    return std::nullopt;
  }

  if (key == keys::PAIR_CONFIG) {
    BlobState state = read_blob<SENDSPIN_PAIR_CONFIG_BLOB_CAP>(this->pair_config_pref_, keys::PAIR_CONFIG, &out);
    if (state == BlobState::PRESENT) {
      return out;
    }
    if (state == BlobState::ABSENT) {
      return this->first_boot_pairing_config_seed_();
    }
    return std::nullopt;
  }

  if (key == keys::LAST_PLAYED) {
    if (read_blob<SENDSPIN_LAST_PLAYED_BLOB_CAP>(this->last_played_pref_, keys::LAST_PLAYED, &out) ==
        BlobState::PRESENT) {
      return out;
    }
    return std::nullopt;
  }

  if (key == keys::STATIC_DELAY) {
    if (read_blob<SENDSPIN_STATIC_DELAY_BLOB_CAP>(this->static_delay_pref_, keys::STATIC_DELAY, &out) ==
        BlobState::PRESENT) {
      return out;
    }
#ifdef USE_SENDSPIN_PLAYER
    // Migrate a pre-encryption raw-uint16 delay stored under the same NVS key; the first
    // new-format save overwrites it.
    LegacyStaticDelayPref legacy{};
    if (this->legacy_static_delay_pref_.load(&legacy)) {
      char buf[SENDSPIN_STATIC_DELAY_BLOB_CAP];
      int n = snprintf(buf, sizeof(buf), "%u", legacy.delay_ms);
      if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
        ESP_LOGI(TAG, "Migrating legacy static delay: %u ms", legacy.delay_ms);
        return std::vector<uint8_t>(buf, buf + n);
      }
    }
#endif
    return std::nullopt;
  }

  ESP_LOGW(TAG, "load_blob: unknown key \"%s\"", key.c_str());
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> SendspinHub::first_boot_pairing_config_seed_() {
  const bool have_seed = this->initial_static_pin_.has_value() || this->initial_pairing_psk_.has_value() ||
                         this->initial_unpaired_access_enabled_;
  if (!have_seed) {
    return std::nullopt;
  }
  // Fail-closed veto mirroring RecordStore::provision_shared_record_if_needed(): a missing
  // config alongside pairing material that WAS written is a damaged store, not a first
  // boot, and seeding there could re-open access a server had revoked. (The library cannot
  // apply this veto itself here because the YAML PSK/PIN seeds above make material look
  // provisioned to it on a genuinely fresh device.)
  namespace keys = sendspin::persistence_keys;
  if (read_blob<SENDSPIN_RECORDS_BLOB_CAP>(this->records_pref_, keys::RECORDS, nullptr) != BlobState::ABSENT ||
      read_blob<SENDSPIN_PAIRING_PSK_BLOB_CAP>(this->pairing_psk_pref_, keys::PAIRING_PSK, nullptr) !=
          BlobState::ABSENT ||
      read_blob<SENDSPIN_STATIC_PIN_BLOB_CAP>(this->static_pin_pref_, keys::STATIC_PIN, nullptr) != BlobState::ABSENT) {
    ESP_LOGW(TAG, "Pairing config missing but other pairing material was stored; ignoring the YAML initial_* seeds");
    return std::nullopt;
  }
  sendspin::SendspinPairingConfig cfg;
  cfg.static_pin_enabled = this->initial_static_pin_.has_value();
  cfg.unpaired_access_enabled = this->initial_unpaired_access_enabled_;
  std::string blob = sendspin::encode_pairing_config(cfg);
  return std::vector<uint8_t>(blob.begin(), blob.end());
}

bool SendspinHub::save_blob(const std::string &key, const uint8_t *data, size_t len) {
  namespace keys = sendspin::persistence_keys;
  if (key == keys::RECORDS) {
    // The only key the library may write from its network thread (pairing finalize).
    // ESPHome's preference queue has no locking, so the write cannot happen here; stage the
    // bytes and let loop() persist them on the main loop a few milliseconds later. The
    // library does not gate anything on this return value, and its in-memory record store is
    // already updated, so the deferral is invisible to pairing.
    {
      std::lock_guard<std::mutex> lock(this->pending_records_mutex_);
      this->pending_records_.assign(data, data + len);
      this->pending_records_valid_ = true;
    }
    return true;
  }

  // Everything below is main-loop-only per the library's provider contract.
  if (key == keys::KEYPAIR) {
    // The keypair is the device identity (client_id derives from it); losing it would
    // invalidate every pairing.
    bool ok = write_blob<SENDSPIN_KEYPAIR_BLOB_CAP>(this->keypair_pref_, keys::KEYPAIR, data, len);
    flush_preferences(keys::KEYPAIR);
    return ok;
  }
  if (key == keys::PAIRING_PSK) {
    // Flag the pairing token for recomputation on the next loop() rather than calling back
    // into the library from a provider method.
    this->pairing_token_dirty_ = true;
    bool ok = write_blob<SENDSPIN_PAIRING_PSK_BLOB_CAP>(this->pairing_psk_pref_, keys::PAIRING_PSK, data, len);
    flush_preferences(keys::PAIRING_PSK);
    return ok;
  }
  if (key == keys::STATIC_PIN) {
    bool ok = write_blob<SENDSPIN_STATIC_PIN_BLOB_CAP>(this->static_pin_pref_, keys::STATIC_PIN, data, len);
    flush_preferences(keys::STATIC_PIN);
    return ok;
  }
  if (key == keys::PAIR_CONFIG) {
    bool ok = write_blob<SENDSPIN_PAIR_CONFIG_BLOB_CAP>(this->pair_config_pref_, keys::PAIR_CONFIG, data, len);
    flush_preferences(keys::PAIR_CONFIG);
    return ok;
  }
  // Not pairing material: let these ride the preference syncer's normal write batching.
  if (key == keys::LAST_PLAYED) {
    return write_blob<SENDSPIN_LAST_PLAYED_BLOB_CAP>(this->last_played_pref_, keys::LAST_PLAYED, data, len);
  }
  if (key == keys::STATIC_DELAY) {
    return write_blob<SENDSPIN_STATIC_DELAY_BLOB_CAP>(this->static_delay_pref_, keys::STATIC_DELAY, data, len);
  }
  ESP_LOGW(TAG, "save_blob: unknown key \"%s\"", key.c_str());
  return false;
}

bool SendspinHub::erase_blob(const std::string &key) {
  namespace keys = sendspin::persistence_keys;
  // A zero-length blob is the erased sentinel: distinct from never-written, so the YAML
  // initial_* seeds cannot resurrect a value a server explicitly cleared.
  if (key == keys::PAIRING_PSK) {
    // See save_blob: recompute the (now absent) pairing token on the next loop.
    this->pairing_token_dirty_ = true;
    bool ok = write_blob<SENDSPIN_PAIRING_PSK_BLOB_CAP>(this->pairing_psk_pref_, keys::PAIRING_PSK, nullptr, 0);
    flush_preferences(keys::PAIRING_PSK);
    return ok;
  }
  if (key == keys::STATIC_PIN) {
    bool ok = write_blob<SENDSPIN_STATIC_PIN_BLOB_CAP>(this->static_pin_pref_, keys::STATIC_PIN, nullptr, 0);
    flush_preferences(keys::STATIC_PIN);
    return ok;
  }
  ESP_LOGW(TAG, "erase_blob: unexpected key \"%s\"", key.c_str());
  return false;
}

// --- Sendspin role specific methods/overrides ---

#ifdef USE_SENDSPIN_ARTWORK
// THREAD CONTEXT: Dedicated artwork decode thread; downstream callbacks run here too
void SendspinHub::on_image_decode(uint8_t slot, const uint8_t *data, size_t length,
                                  sendspin::SendspinImageFormat format) {
  this->artwork_image_decode_callbacks_.call(slot, data, length, format);
}

// THREAD CONTEXT: Main loop (fired from client_->loop() once the slot's offset-shifted display
// deadline is reached; lateness_ms reports how far past the deadline the display slipped)
void SendspinHub::on_image_display(uint8_t slot, uint32_t lateness_ms) {
  this->artwork_image_display_callbacks_.call(slot, lateness_ms);
}

// THREAD CONTEXT: Main loop (fired from client_->loop())
void SendspinHub::on_image_clear(uint8_t slot) { this->artwork_image_clear_callbacks_.call(slot); }

// THREAD CONTEXT: Main loop (invoked from SendspinImageSlot once a delivery is fully presented)
void SendspinHub::artwork_frame_done(uint8_t slot) {
  if (this->artwork_role_ != nullptr) {
    this->artwork_role_->frame_done(slot);
  }
}
#endif

#ifdef USE_SENDSPIN_CONTROLLER
// THREAD CONTEXT: Main loop (invoked from ESPHome actions / other components)
void SendspinHub::send_client_command(sendspin::SendspinControllerCommand command, std::optional<uint8_t> volume,
                                      std::optional<bool> mute) {
  if (this->is_ready()) {
    sendspin::ClientCommandControllerObject obj = {
        .command = command,
        .volume = volume,
        .muted = mute,
    };
    this->controller_role_->send_command(obj);
  }
}

// THREAD CONTEXT: Main loop (ControllerRoleListener override, fired from client_->loop())
void SendspinHub::on_controller_state(const sendspin::ServerStateControllerObject &state) {
  this->controller_state_callbacks_.call(state);
}

// THREAD CONTEXT: Main loop (ControllerRoleListener override, fired from client_->loop())
// Unlike metadata, this cannot be fanned out as a default-constructed state object: volume and muted are plain values
// rather than optionals, so children would read a real-looking 0% volume where we mean no value at all. A separate
// callback lets each child clear only what it can represent.
void SendspinHub::on_controller_state_clear() { this->controller_state_clear_callbacks_.call(); }
#endif

#ifdef USE_SENDSPIN_METADATA
// THREAD CONTEXT: Main loop (MetadataRoleListener override, fired from client_->loop())
void SendspinHub::on_metadata(const sendspin::ServerMetadataStateObject &metadata) {
  this->metadata_update_callbacks_.call(metadata);
}

// THREAD CONTEXT: Main loop (MetadataRoleListener override, fired from client_->loop())
// The cached metadata was dropped because the connection to the server was lost, so what the children now mirror is
// the empty state. Fanning that out as a default-constructed state object rather than through a separate callback
// keeps one code path in the children: every field is nullopt, which they already publish as empty/unknown.
void SendspinHub::on_metadata_clear() { this->metadata_update_callbacks_.call(sendspin::ServerMetadataStateObject{}); }

// THREAD CONTEXT: Main loop (invoked from Sendspin components)
uint32_t SendspinHub::get_track_progress_ms() const {
  if (this->is_ready()) {
    return this->metadata_role_->get_track_progress_ms();
  }
  return 0;
}
#endif

#ifdef USE_SENDSPIN_PLAYER
// THREAD CONTEXT: Main loop, called from child component setup() after player role is created and configured
sendspin::PlayerRole *SendspinHub::get_player_role() {
  if (this->is_ready()) {
    return this->client_->player();
  }
  return nullptr;
}

#endif

}  // namespace esphome::sendspin_

#endif  // USE_ESP32
