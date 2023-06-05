#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "dabalite.h"

namespace esphome {
namespace statistics {


// // partial summary statistics structure
// struct Partial {
//   float max;
//   float min;

//   double m2;      // used to find variance/standard deviation via Welford's algorithm

//   float mean;

//   size_t count;
// };

class StatisticsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;

  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }
  void set_sd_sensor(sensor::Sensor *sd_sensor) { this->sd_sensor_ = sd_sensor; }

  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }


 protected:
  void handle_new_value_(float value);

  sensor::Sensor *source_sensor_{nullptr};

  sensor::Sensor *mean_sensor_{nullptr};
  sensor::Sensor *max_sensor_{nullptr};  
  sensor::Sensor *min_sensor_{nullptr};  
  sensor::Sensor *sd_sensor_{nullptr};

  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  DABALite *queue_{nullptr};

  // functions to convert a Partial structure to usable summary statistics
  float lower_mean(Partial c) {
    return c.mean;
  }

  float lower_max(Partial c) {
    return c.max;
  }

  float lower_min(Partial c) {
    return c.min;
  }

  float lower_sd(Partial c) {
    return std::sqrt(lower_variance(c));      
  }

  float lower_variance(Partial c) {
    // Welford's algorithm for variance
    return c.m2/(static_cast<double>(c.count)-1);
  }  
};

}  // namespace statistics
}  // namespace esphome
