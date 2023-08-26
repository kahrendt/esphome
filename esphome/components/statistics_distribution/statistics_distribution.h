/*
 * Uses a TDigest (https://arxiv.org/pdf/1902.04023.pdf) for accurate quantile approximations of a sensors output
 *
 * Implemented by Kevin Ahrendt for the ESPHome project, August of 2023
 */

#pragma once

#include "merging_tdigest.h"

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics_distribution {

struct QuantileSensor {
  sensor::Sensor *sensor;
  float quantile;
};

struct CDFSensor {
  sensor::Sensor *sensor;
  float value;
};

class StatisticsDistributionComponent : public PollingComponent {
 public:
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void dump_config() override;

  void setup() override;

  void update() override;

  /// @brief Reset the window by clearing it.
  void reset();

  // Source sensor of measurements
  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_compression(uint16_t compression) { this->compression_ = compression; }
  void set_buffer_size(uint16_t buffer_size) { this->buffer_size_ = buffer_size; }
  void set_scale_function(ScaleFunctions scale_function) { this->scale_function_ = scale_function; }

  void add_quantile_sensor(sensor::Sensor *quantile_sensor, float quantile);
  void add_cdf_sensor(sensor::Sensor *cdf_sensor, float value);

  void set_total_weight_sensor(sensor::Sensor *weight_sensor) { this->total_weight_sensor_ = weight_sensor; }

  void set_hash(const std::string &config_id) { this->hash_ = fnv1_hash("statistics_component_" + config_id); }
  void set_restore(bool restore) { this->restore_ = restore; }

 protected:
  // Source sensor of measurement data
  sensor::Sensor *source_sensor_{nullptr};

  // T Digest for approximating quantiles
  MergingDigest *tdigest_{nullptr};
  ScaleFunctions scale_function_{};
  uint16_t compression_{100};
  uint16_t buffer_size_{10};

  sensor::Sensor *total_weight_sensor_;

  ESPPreferenceObject pref_metadata_{};
  ESPPreferenceObject pref_centroids_{};
  uint32_t hash_{};
  bool restore_{};

  std::vector<QuantileSensor> quantile_sensors_{};
  std::vector<CDFSensor> cdf_sensors_{};

  //////////////////////
  // Internal Methods //
  //////////////////////

  /// @brief Insert new sensor measurements and update sensors.
  void handle_new_value_(float value);
};

}  // namespace statistics_distribution
}  // namespace esphome
