
#pragma once

#include "esp_radar.h"
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include <freertos/FreeRTOS.h>
#include <freertos/FreeRTOSConfig.h>
#include <freertos/event_groups.h>
#include <esp_mac.h>
#include <rom/ets_sys.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

#define RX_BUFFER_SIZE 1460
#define KEEPALIVE_IDLE 1
#define KEEPALIVE_INTERVAL 1
#define KEEPALIVE_COUNT 3

namespace esphome {
namespace wifi_csi {

static const char *const TAG = "wifi_csi";
static QueueHandle_t radar_info_queue_ = NULL;

static void wifi_radar_callback_(const wifi_radar_info_t *info, void *ctx) {
  // ESP_LOGI(TAG, "radar callback");
  wifi_radar_info_t *radar_info = (wifi_radar_info_t *) malloc(sizeof(wifi_radar_info_t));
  memcpy(radar_info, info, sizeof(wifi_radar_info_t));

  if (xQueueSend(radar_info_queue_, &radar_info, 0) == pdFALSE) {
    ESP_LOGW(TAG, "Radar info queue is full");
    free(radar_info);
  }
}

class WiFiCSIComponent : public Component {
 public:
  void setup() override {
    ESP_LOGCONFIG("wifi_csi", "Setting up WiFi CSI");

    // wifi_csi_config_t csi_conf = {
    //     .lltf_en = true,
    //     .htltf_en = false,
    //     .stbc_htltf2_en = false,
    //     .ltf_merge_en = true,
    //     .channel_filter_en = true,
    //     .manu_scale = true,
    //     .shift = true,
    // };

    // wifi_radar_config_t radar_config = WIFI_RADAR_CONFIG_DEFAULT();
    // radar_config.wifi_radar_cb = wifi_radar_callback_;
    // wifi_radar_config_t radar_config = {
    //     .filter_mac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},  // No filtering based on MAC address
    //     // .filter_dmac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    //     // .filter_len = 0,
    //     .wifi_radar_cb = this->wifi_radar_callback_,
    //     // .wifi_radar_cb_ctx = NULL,
    //     // .wifi_csi_filtered_cb = NULL,
    //     // .wifi_csi_cb_ctx = NULL,
    //     .csi_config = csi_conf,
    // };
    // wifi_csi_config_t csi_config = {
    //     .lltf_en = true,
    //     .htltf_en = false,
    //     .stbc_htltf2_en = false,
    //     .ltf_merge_en = true,
    //     .channel_filter_en = true,
    //     .manu_scale = true,
    //     .shift = true,
    // };

    // // static wifi_ap_record_t s_ap_info = {0};
    // ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&s_ap_info));
    // ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    // ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, s_ap_info.bssid));
    // ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    radar_info_queue_ = xQueueCreate(100, sizeof(wifi_radar_info_t *));

    // wifi_radar_config_t conf = {
    //     .filter_mac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},  // No filtering based on MAC address
    //     .wifi_radar_cb = wifi_radar_callback_,
    // };

    // wifi::bssid_t bssid = wifi::global_wifi_component->wifi_bssid();
    wifi_radar_config_t conf = {
        .filter_mac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        // .filter_mac = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00},
        // .filter_mac = {0xd0, 0x21, 0xf9, 0xbf, 0x2a, 0xc4},
        .filter_dmac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        .wifi_radar_cb = wifi_radar_callback_,
        .wifi_radar_cb_ctx = NULL,
        .wifi_csi_filtered_cb = NULL,
        .wifi_csi_cb_ctx = NULL,
        .csi_handle_priority = 5,
        .csi_combine_priority = 5,
        .csi_recv_interval = 10,
        .csi_handle_time = 250,
        .csi_config =
            {
                .lltf_en = true,
                .htltf_en = false,
                .stbc_htltf2_en = false,
                .ltf_merge_en = true,
                .channel_filter_en = false,
                .manu_scale = true,
                .shift = true,
            },
    };

    // memcpy(conf.filter_mac, bssid.data(), sizeof(bssid));

    wifi_ping_router_start();
    // if (esp_radar_init())
    //  if ((esp_radar_set_config(&conf)))
    //   if (esp_radar_start())
    //   this->publish_state(2);
    // else
    //   this->publish_state(5);

    // wifi_ping_router_start();
    esp_radar_init();
    esp_radar_set_config(&conf);
    esp_radar_start();

    start_training_();

    this->set_timeout("train", 10 * 1000, [this]() { this->stop_training_(); });
  };

  // static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
  //   if (!info || !info->buf || !info->mac) {
  //     ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
  //     return;
  //   }

  //   if (memcmp(info->mac, ctx, 6)) {
  //     return;
  //   }

  //   static uint32_t s_count = 0;
  //   const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;

  //   if (!s_count) {
  //     ESP_LOGI(TAG, "================ CSI RECV ================");
  //     ets_printf(
  //         "type,seq,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_"
  //         "floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,len,first_word,data\n");
  //   }

  //   /** Only LLTF sub-carriers are selected. */
  //   info->len = 128;

  //   printf("CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", s_count++,
  //          MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate, rx_ctrl->sig_mode, rx_ctrl->mcs, rx_ctrl->cwb,
  //          rx_ctrl->smoothing, rx_ctrl->not_sounding, rx_ctrl->aggregation, rx_ctrl->stbc, rx_ctrl->fec_coding,
  //          rx_ctrl->sgi, rx_ctrl->noise_floor, rx_ctrl->ampdu_cnt, rx_ctrl->channel, rx_ctrl->secondary_channel,
  //          rx_ctrl->timestamp, rx_ctrl->ant, rx_ctrl->sig_len, rx_ctrl->rx_state);

  //   printf(",%d,%d,\"[%d", info->len, info->first_word_invalid, info->buf[0]);

  //   for (int i = 1; i < info->len; i++) {
  //     printf(",%d", info->buf[i]);
  //   }

  //   printf("]\"\n");
  // }

  void loop() override {
    wifi_radar_info_t *radar_info;
    while (xQueueReceive(radar_info_queue_, &radar_info, 0)) {
      detect_presence(radar_info);

      free(radar_info);
    }
  };

  void dump_config() override { ESP_LOGCONFIG(TAG, "WiFi-CSI Component"); };
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_motion_sensor(binary_sensor::BinarySensor *motion_sensor) { this->motion_sensor_ = motion_sensor; }
  void set_presence_sensor(binary_sensor::BinarySensor *presence_sensor) { this->presence_sensor_ = presence_sensor; }

 protected:
  wifi_radar_info_t detection_threshold_{};

  wifi_radar_info_t measurements_[10];

  float wander_buffer_[10];
  float jitter_buffer_[10];

  binary_sensor::BinarySensor *motion_sensor_{nullptr};
  binary_sensor::BinarySensor *presence_sensor_{nullptr};

  // xQueueHandle radar_info_queue_{};

  bool training_in_process_{false};
  bool current_motion_{false};

  uint32_t measurements_count_{0};

  float waveform_jitter_threshold_{0.002};
  float waveform_wander_threshold_{0.002};

  void start_training_() {
    this->training_in_process_ = true;

    esp_radar_train_remove();
    esp_radar_train_start();

    ESP_LOGI(TAG, "Started training");
  }

  void stop_training_() {
    if (!training_in_process_) {
      ESP_LOGW(TAG, "Can't stop training as it has not started");
      return;
    }

    esp_radar_train_stop(&this->waveform_jitter_threshold_, &this->waveform_wander_threshold_);
    this->training_in_process_ = false;

    this->waveform_jitter_threshold_ = this->waveform_jitter_threshold_ * 1.1;
    this->waveform_wander_threshold_ = this->waveform_wander_threshold_ * 1.1;

    ESP_LOGI(TAG, "Stopped training and updated thresholds; %.4f, %.4f", this->waveform_jitter_threshold_,
             this->waveform_wander_threshold_);
  }
  void detect_presence(const wifi_radar_info_t *info) {
    if (this->training_in_process_)
      return;

    if (this->waveform_jitter_threshold_ == 0)
      return;

    // this->measurements_[++this->measurements_count_ % 10] = *info;
    this->jitter_buffer_[this->measurements_count_ % 10] = info->waveform_jitter;
    this->wander_buffer_[this->measurements_count_ % 10] = info->waveform_wander;
    ++this->measurements_count_;

    if (this->measurements_count_ < 10)
      return;

    uint8_t presence_count = 0;
    uint8_t motion_count = 0;

    for (int i = 0; i < 10; i++) {
      if (this->wander_buffer_[i] > this->waveform_wander_threshold_) {
        ++presence_count;
      }
      if (this->jitter_buffer_[i] > this->waveform_jitter_threshold_) {
        ++motion_count;
      }
    }

    if (presence_count < 2) {
      this->presence_sensor_->publish_state(false);
    } else {
      this->presence_sensor_->publish_state(true);
    }

    if (motion_count < 2) {
      this->motion_sensor_->publish_state(false);
    } else {
      this->motion_sensor_->publish_state(true);
    }
  }

  // malloc(sizeof(wifi_radar_info_t));
  // memcpy(radar_info, info, sizeof(wifi_radar_info_t));

  // if (xQueueSend(g_radar_info_queue, &radar_info, 0) == pdFALSE) {
  //   ESP_LOGW(TAG, "Radar info queue is full");
  //   free(radar_info);
  // }
  static esp_err_t wifi_ping_router_start() {
    static esp_ping_handle_t ping_handle = NULL;
    esp_ping_config_t ping_config = {
        .count = 0,
        .interval_ms = 1000 / 100,
        .timeout_ms = 1000,
        .data_size = 1,
        .tos = 0,
        .task_stack_size = 4096,
        .task_prio = 0,
    };

    esp_netif_ip_info_t local_ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
    ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
    inet_addr_to_ip4addr(ip_2_ip4(&ping_config.target_addr), (struct in_addr *) &local_ip.gw);

    esp_ping_callbacks_t cbs = {0};
    esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    esp_ping_start(ping_handle);

    return ESP_OK;
  }
};  // namespace wifi_csi

}  // namespace wifi_csi
}  // namespace esphome
