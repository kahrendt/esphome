#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "daba_lite.h"
#include "esphome/core/log.h"
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
  void set_sd_sensor(sensor::Sensor *sd_sensor) { this->sd_sensor_ = sd_sensor; }
  void set_variance_sensor(sensor::Sensor *variance_sensor) { this->variance_sensor_ = variance_sensor; }
  void set_count_sensor(sensor::Sensor *count_sensor) { this->count_sensor_ = count_sensor; }
  void set_trend_sensor(sensor::Sensor *trend_sensor) { this->trend_sensor_ = trend_sensor; }
  void set_covariance_sensor(sensor::Sensor *covariance_senesor) { this->covariance_sensor_ = covariance_senesor; }

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
  sensor::Sensor *variance_sensor_{nullptr};
  sensor::Sensor *count_sensor_{nullptr};
  sensor::Sensor *trend_sensor_{nullptr};
  sensor::Sensor *covariance_sensor_{nullptr};

  size_t window_size_{};
  size_t send_every_{};
  size_t send_at_{};

  DABALite partial_stats_queue_{};

  Aggregate current_statistics_{};

  void update_current_statistics_();

  float mean_() { return this->current_statistics_.mean; }
  float max_() { return this->current_statistics_.max; }
  float min_() { return this->current_statistics_.min; }
  float variance_() {
    // Welford's algorithm for variance
    return this->current_statistics_.m2 / (static_cast<double>(this->current_statistics_.count) - 1);
  }
  float sd_() { return std::sqrt(this->variance_()); }
  size_t count_() { return this->current_statistics_.count; }

  float covariance_() { return this->current_statistics_.c2 / (this->current_statistics_.count - 1); }

  // slope for line of best fit; i.e., trend is covariance(x,y)/variance(x)
  //   - the variance and covariance are obtained by taking m2/(n-1) and c2/(n-1) respectivally
  //   - we skip the division of the common factor of (n-1) in the numerator and denominator
  float trend_() { return this->current_statistics_.c2 / this->current_statistics_.x_m2; }
};

}  // namespace statistics
}  // namespace esphome
