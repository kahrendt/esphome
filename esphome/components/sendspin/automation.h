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

}  // namespace esphome::sendspin_

#endif  // USE_ESP32
