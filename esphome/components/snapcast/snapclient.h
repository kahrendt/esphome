#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK

#include "median_filter.h"

#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/socket/socket.h"

#include <freertos/queue.h>

namespace esphome {
namespace snapcast {

class Snapclient {
 public:
  Snapclient(QueueHandle_t chunk_data_queue)
      : chunk_data_queue_(chunk_data_queue), network_latency_filter_(MedianFilter(50)){};

  esp_err_t connect_to_server();
  void disconnect_from_server();

  esp_err_t send_client_message();
  esp_err_t send_hello_message();
  esp_err_t send_time_message();

  esp_err_t process_messages();

 protected:
  QueueHandle_t chunk_data_queue_;
  std::unique_ptr<socket::Socket> socket_;
  MedianFilter network_latency_filter_;
};
}  // namespace snapcast
}  // namespace esphome
#endif
