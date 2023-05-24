#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics {

class StatisticsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;

  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }

  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }
 protected:
  sensor::Sensor *source_sensor_{nullptr};

  void handle_new_value_(float value);

  sensor::Sensor *mean_sensor_{nullptr};
  sensor::Sensor *max_sensor_{nullptr};  
  sensor::Sensor *min_sensor_{nullptr};  

  std::vector<float> queue_;
  size_t index_ = 0;

  //std::deque<float> queue_;
  float Ex;
  float Ex2;

  size_t update_count_ = 0;
  size_t valid_count_ = 0;
  size_t sum_ = 0;

  size_t window_size_;
  size_t send_every_;
  size_t send_at_;
};

}  // namespace statistics
}  // namespace esphome
