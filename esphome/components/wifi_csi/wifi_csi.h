
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/statistics/aggregate.h"
#include "esphome/components/statistics/aggregate_queue.h"
#include "esphome/components/statistics/daba_lite_queue.h"

#include <esp_mac.h>
#include <rom/ets_sys.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <ping/ping_sock.h>

namespace esphome {
namespace wifi_csi {

static constexpr statistics::StatisticsCalculationConfig stats_conf = {
    .group_type = statistics::GROUP_TYPE_SAMPLE,
    .weight_type = statistics::WEIGHT_TYPE_SIMPLE,
};

static std::vector<statistics::Aggregate> channel_aggregates;

class WiFiCSIComponent : public PollingComponent {
 public:
  void setup() override;

  void update() override;

  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_motion_sensor(binary_sensor::BinarySensor *motion_sensor) { this->motion_sensor_ = motion_sensor; }
  void set_presence_sensor(binary_sensor::BinarySensor *presence_sensor) { this->presence_sensor_ = presence_sensor; }
  void set_training_switch(switch_::Switch *training_switch) { this->training_switch_ = training_switch; }

  void set_jitter_sensor(sensor::Sensor *jitter_sensor) { this->jitter_sensor_ = jitter_sensor; }
  void set_wander_sensor(sensor::Sensor *wander_sensor) { this->wander_sensor_ = wander_sensor; }

 protected:
  binary_sensor::BinarySensor *motion_sensor_{nullptr};
  binary_sensor::BinarySensor *presence_sensor_{nullptr};
  sensor::Sensor *jitter_sensor_{nullptr};
  sensor::Sensor *wander_sensor_{nullptr};

  switch_::Switch *training_switch_{nullptr};

  std::vector<statistics::DABALiteQueue> amplitude_queues_;

  static void wifi_csi_rx_callback(void *ctx, wifi_csi_info_t *info);

  void wifi_csi_init_();

  static void wifi_ping_router_start_();
};

}  // namespace wifi_csi
}  // namespace esphome
