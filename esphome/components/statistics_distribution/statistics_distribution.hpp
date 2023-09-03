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

static const char *const TAG = "statistics_distribution";

struct QuantileSensor {
  sensor::Sensor *sensor;
  float quantile;
};

struct CDFSensor {
  sensor::Sensor *sensor;
  float value;
};

template<uint8_t SZ> class StatisticsDistributionComponent : public PollingComponent, public sensor::Sensor {
 public:
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Statistics Distribution Component:");

    LOG_SENSOR("  ", "Source Sensor:", this->source_sensor_);
  }

  void setup() override {
    this->tdigest_ = new MergingDigest(SZ / 2, this->scale_function_);

    if (this->restore_) {
      this->pref_centroids_ = global_preferences->make_preference<Centroid[SZ]>(this->get_object_id_hash());

      Centroid saved_tdigest[SZ];

      if (this->pref_centroids_.load(&saved_tdigest)) {
        uint8_t i = 0;
        while ((i < SZ) && (saved_tdigest[i].get_weight() > 0)) {
          this->tdigest_->add(saved_tdigest[i].get_mean(), saved_tdigest[i].get_weight());
          ++i;
        }

        this->update();
      }
    }

    // On every source sensor update, call handle_new_value_()
    this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
  }

  void update() override {
    this->tdigest_->merge_new_values();
    for (auto quantile_sensor : this->quantile_sensors_) {
      float quant = this->tdigest_->quantile(quantile_sensor.quantile);
      quantile_sensor.sensor->publish_state(quant);
    }

    for (auto cdf_sensor : this->cdf_sensors_) {
      float cdf = this->tdigest_->cdf(cdf_sensor.value);
      cdf_sensor.sensor->publish_state(cdf);
    }

    if (this->total_weight_sensor_) {
      this->total_weight_sensor_->publish_state(this->tdigest_->get_total_weight());
    }

    Centroid tdigest[SZ];
    this->tdigest_->compress_for_saving(SZ, tdigest);
    this->pref_centroids_.save(&tdigest);
  }

  /// @brief Reset the window by clearing it.
  void reset() { this->tdigest_->clear(); }

  // Source sensor of measurements
  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_scale_function(ScaleFunctions scale_function) { this->scale_function_ = scale_function; }

  void add_quantile_sensor(sensor::Sensor *quantile_sensor, float quantile) {
    QuantileSensor sensor{
        .sensor = quantile_sensor,
        .quantile = quantile,
    };
    this->quantile_sensors_.push_back(sensor);
  }

  void add_cdf_sensor(sensor::Sensor *cdf_sensor, float value) {
    CDFSensor sensor{
        .sensor = cdf_sensor,
        .value = value,
    };
    this->cdf_sensors_.push_back(sensor);
  }

  void set_total_weight_sensor(sensor::Sensor *weight_sensor) { this->total_weight_sensor_ = weight_sensor; }

  void set_hash(const std::string &config_id) { this->hash_ = fnv1_hash("statistics_component_" + config_id); }
  void set_restore(bool restore) { this->restore_ = restore; }

 protected:
  // Source sensor of measurement data
  sensor::Sensor *source_sensor_{nullptr};

  // T Digest for approximating quantiles
  MergingDigest *tdigest_{nullptr};
  ScaleFunctions scale_function_{};

  sensor::Sensor *total_weight_sensor_;

  ESPPreferenceObject pref_centroids_{};
  uint32_t hash_{};
  bool restore_{};

  std::vector<QuantileSensor> quantile_sensors_{};
  std::vector<CDFSensor> cdf_sensors_{};

  //////////////////////
  // Internal Methods //
  //////////////////////

  /// @brief Insert new sensor measurements and update sensors.
  void handle_new_value_(float value) {
    this->tdigest_->add(value, 1);
    this->publish_state(this->tdigest_->cdf(value));
  }
};

// template<typename... Ts> class StatisticsDistributionCondition : public Condition<Ts...> {
//  public:
//   void set_low_threshold(float low_threshold) { this->low_threshold_ = low_threshold; }
//   void set_high_threshold(float high_threshold) { this->high_threshold_ = high_threshold; }

//   bool check(Ts... x) override {
//     double elevation = this->elevation_.value(x...);
//     double current = this->parent_->elevation();
//     if (this->above_) {
//       return current > elevation;
//     } else {
//       return current < elevation;
//     }
//   }

//  protected:
//   float low_threshold_;
//   float high_threshold_;
// };

}  // namespace statistics_distribution
}  // namespace esphome
