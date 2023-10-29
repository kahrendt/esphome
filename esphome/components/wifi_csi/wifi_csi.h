
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/statistics/aggregate.h"
#include "esphome/components/statistics/continuous_singular.h"

#include <complex>

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

static const char *const TAG = "wifi_csi";

static statistics::StatisticsCalculationConfig stats_conf_ = {
    .group_type = statistics::GROUP_TYPE_SAMPLE,
    .weight_type = statistics::WEIGHT_TYPE_SIMPLE,
};

static std::unique_ptr<statistics::Aggregate> running_chunk_aggregate_ =
    make_unique<statistics::Aggregate>(stats_conf_);
;

class WiFiCSIComponent : public PollingComponent {
 public:
  void setup() override {
    ESP_LOGCONFIG("wifi_csi", "Setting up WiFi CSI");

    // this->running_chunk_aggregate_ = make_unique<statistics::Aggregate>(stats_conf_);

    wifi_ping_router_start();
    wifi_csi_init();
  };

  void update() override {
    this->jitter_sensor_->publish_state(running_chunk_aggregate_->get_mean());
    this->wander_sensor_->publish_state(running_chunk_aggregate_->get_count());

    *running_chunk_aggregate_ = statistics::Aggregate(stats_conf_);
  };

  void dump_config() override { ESP_LOGCONFIG(TAG, "WiFi-CSI Component"); };
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_motion_sensor(binary_sensor::BinarySensor *motion_sensor) { this->motion_sensor_ = motion_sensor; }
  void set_presence_sensor(binary_sensor::BinarySensor *presence_sensor) { this->presence_sensor_ = presence_sensor; }
  void set_training_switch(switch_::Switch *training_switch) { this->training_switch_ = training_switch; }

  void set_jitter_sensor(sensor::Sensor *jitter_sensor) { this->jitter_sensor_ = jitter_sensor; }
  void set_wander_sensor(sensor::Sensor *wander_sensor) { this->wander_sensor_ = wander_sensor; }

 protected:
  void wifi_csi_init() {
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = true,
        .shift = true,
    };

    static wifi_ap_record_t s_ap_info = {0};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&s_ap_info));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(this->wifi_csi_rx_callback, s_ap_info.bssid));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
  }

  binary_sensor::BinarySensor *motion_sensor_{nullptr};
  binary_sensor::BinarySensor *presence_sensor_{nullptr};
  sensor::Sensor *jitter_sensor_{nullptr};
  sensor::Sensor *wander_sensor_{nullptr};

  switch_::Switch *training_switch_{nullptr};

  static void wifi_csi_rx_callback(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) {
      ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
      return;
    }

    if (memcmp(info->mac, ctx, 6)) {
      return;
    }

    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;

    /** Only LLTF sub-carriers are selected. */
    info->len = 128;

    std::complex<float> subcarrier_1(info->buf[2], info->buf[3]);
    statistics::Aggregate new_value =
        statistics::Aggregate(stats_conf_, std::abs(subcarrier_1), 1, rx_ctrl->timestamp, 0);

    *running_chunk_aggregate_ = running_chunk_aggregate_->combine_with(new_value);
    // amp_subcarrier_1_aggregate = amp_subcarrier_1_aggregate.combine_with(new_value);
  }

  static void wifi_ping_router_start() {
    static esp_ping_handle_t ping_handle = NULL;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.count = 0;
    ping_config.interval_ms = 1000 / 100;
    ping_config.task_stack_size = 3072;
    ping_config.data_size = 1;

    esp_netif_ip_info_t local_ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
    // ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
    char buf[32];
    sprintf(buf, IPSTR, IP2STR(&local_ip.gw));
    ip_addr_t gw_addr;
    ip4addr_aton(buf, &gw_addr);
    ping_config.target_addr = gw_addr;
    // ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
    // ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;

    esp_ping_callbacks_t cbs = {0};
    esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    esp_ping_start(ping_handle);
  }
};  // namespace wifi_csi

}  // namespace wifi_csi
}  // namespace esphome
