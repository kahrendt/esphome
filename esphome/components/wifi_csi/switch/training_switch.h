
#pragma once

#include "esphome/components/switch/switch.h"
#include "../wifi_csi.h"

namespace esphome {
namespace wifi_csi {

class TrainingSwitch : public switch_::Switch, public Parented<WiFiCSIComponent> {
 public:
  TrainingSwitch() = default;

 protected:
  void write_state(bool state) override {
    this->publish_state(state);
    this->parent_->set_training(state);
  };
};  // namespace wifi_csi

}  // namespace wifi_csi
}  // namespace esphome
