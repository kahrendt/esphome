#include "statistics_distribution.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace statistics_distribution {

static const char *const TAG = "statistics_distribution";

static const uint8_t MAX_CENTROIDS_FOR_FLASH = 100;

//////////////////////////
// Public Class Methods //
//////////////////////////

void StatisticsDistributionComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Statistics Distribution Component:");

  LOG_SENSOR("  ", "Source Sensor:", this->source_sensor_);
}

void StatisticsDistributionComponent::setup() {
  this->tdigest_ = new MergingDigest(this->compression_, this->scale_function_, this->buffer_size_);

  this->pref_centroids_ = global_preferences->make_preference<Centroid[100]>(317);

  Centroid saved_tdigest[MAX_CENTROIDS_FOR_FLASH];

  if (this->pref_centroids_.load(&saved_tdigest)) {
    uint8_t i = 0;
    while ((i < MAX_CENTROIDS_FOR_FLASH) && (saved_tdigest[i].get_weight() > 0)) {
      this->tdigest_->add(saved_tdigest[i].get_mean(), saved_tdigest[i].get_weight());
      ++i;
    }

    this->update();
  }

  // On every source sensor update, call handle_new_value_()
  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}

void StatisticsDistributionComponent::update() {
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

  Centroid tdigest[MAX_CENTROIDS_FOR_FLASH];
  this->tdigest_->compress_for_saving(MAX_CENTROIDS_FOR_FLASH, tdigest);
  this->pref_centroids_.save(&tdigest);
}

void StatisticsDistributionComponent::add_quantile_sensor(sensor::Sensor *quantile_sensor, float quantile) {
  QuantileSensor sensor{
      .sensor = quantile_sensor,
      .quantile = quantile,
  };
  this->quantile_sensors_.push_back(sensor);
}

void StatisticsDistributionComponent::add_cdf_sensor(sensor::Sensor *cdf_sensor, float value) {
  CDFSensor sensor{
      .sensor = cdf_sensor,
      .value = value,
  };
  this->cdf_sensors_.push_back(sensor);
}

void StatisticsDistributionComponent::reset() { this->tdigest_->clear(); }

//////////////////////
// Internal Methods //
//////////////////////

void StatisticsDistributionComponent::handle_new_value_(float value) { this->tdigest_->add(value, 1); }

}  // namespace statistics_distribution
}  // namespace esphome
