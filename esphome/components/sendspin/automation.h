#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "sendspin_hub.h"

namespace esphome::sendspin_ {

#ifdef USE_SENDSPIN_CONTROLLER
template<typename... Ts> class SendspinSwitchCommandAction final : public Action<Ts...>, public Parented<SendspinHub> {
 public:
  void play(const Ts &...x) override {
    // Clear any EXTERNAL_SOURCE state so the switch command is followed
    this->parent_->update_state(sendspin::SendspinClientState::SYNCHRONIZED);
    this->parent_->send_client_command(sendspin::SendspinControllerCommand::SWITCH);
  }
};
#endif  // USE_SENDSPIN_CONTROLLER

/// @brief Action that confirms the operator pairing-window gesture for gesture-gated PIN pairing.
template<typename... Ts>
class SendspinConfirmPairingWindowAction final : public Action<Ts...>, public Parented<SendspinHub> {
 public:
  void play(const Ts &...x) override { this->parent_->confirm_pairing_window(); }
};

/// @brief Trigger fired when the library requests the operator open the pairing window.
class SendspinOpenPairingWindowTrigger : public Trigger<> {
 public:
  explicit SendspinOpenPairingWindowTrigger(SendspinHub *hub) {
    hub->add_on_open_pairing_window_callback([this]() { this->trigger(); });
  }
};

/// @brief Trigger fired when the pairing-window prompt should be dismissed.
class SendspinClosePairingWindowTrigger : public Trigger<> {
 public:
  explicit SendspinClosePairingWindowTrigger(SendspinHub *hub) {
    hub->add_on_close_pairing_window_callback([this]() { this->trigger(); });
  }
};

/// @brief Trigger fired when a dynamic pairing PIN should be shown to the user.
/// The PIN is a zero-padded decimal string (e.g., "042735").
class SendspinDisplayPairingPinTrigger : public Trigger<std::string> {
 public:
  explicit SendspinDisplayPairingPinTrigger(SendspinHub *hub) {
    hub->add_on_display_pairing_pin_callback([this](const std::string &pin) { this->trigger(pin); });
  }
};

/// @brief Trigger fired when the dynamic pairing PIN should be cleared from the display.
class SendspinClearPairingPinTrigger : public Trigger<> {
 public:
  explicit SendspinClearPairingPinTrigger(SendspinHub *hub) {
    hub->add_on_clear_pairing_pin_callback([this]() { this->trigger(); });
  }
};

/// @brief Trigger fired when a pairing exchange completes and the long-term record is stored.
/// server_id is the base64url public key of the newly paired server.
class SendspinPairingSucceededTrigger : public Trigger<std::string> {
 public:
  explicit SendspinPairingSucceededTrigger(SendspinHub *hub) {
    hub->add_on_pairing_succeeded_callback([this](const std::string &server_id) { this->trigger(server_id); });
  }
};

/// @brief Trigger fired when a pairing exchange is aborted.
/// server_id identifies the server; reason is a wire-style string (e.g., "pin_mismatch").
class SendspinPairingFailedTrigger : public Trigger<std::string, std::string> {
 public:
  explicit SendspinPairingFailedTrigger(SendspinHub *hub) {
    hub->add_on_pairing_failed_callback(
        [this](const std::string &server_id, const std::string &reason) { this->trigger(server_id, reason); });
  }
};

}  // namespace esphome::sendspin_

#endif  // USE_ESP32
