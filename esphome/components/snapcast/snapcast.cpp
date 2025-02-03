#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/log.h"

// #include "mdns.h"

#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "esp_mac.h"

#include <esp_timer.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

static void close_connection(struct netconn *conn) {
  if (conn != nullptr) {
    netconn_close(conn);
    netconn_delete(conn);
    conn = nullptr;
  }
}

void SnapcastPlayer::setup() {
  xTaskCreate(snapcast_task, "snapcast", 1024 * 4, (void *) this, 1, &this->snapcast_task_handle_);
}

void SnapcastPlayer::loop() {}

void SnapcastPlayer::snapcast_task(void *params) {  // // Find snapcast server
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  while (true) {
    // // Connect to first snapcast server found

    // mdns_result_t *mdns_result;

    // mdns_init();
    // ESP_LOGI(TAG, "Lookup snapcast service on network");
    // esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &mdns_result);

    // if (!mdns_result) {
    //   ESP_LOGW(TAG, "No results found for snapcast service!");
    // }

    // if (mdns_result->addr) {
    //   ip_addr_t *remote_ip;
    //   ip_addr_copy(remote_ip, (mdns_result->addr->addr));
    //   remote_ip.type = IPADDR_TYPE_V4;
    //   remotePort = r->port;
    //   ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);
    // }
    // mdns_query_results_free(mdns_result);

    // this->socket_ = socket::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    // struct sockaddr_storage server;

    // socklen_t sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), "192.168.1.35", 1704);
    // if (sl == 0) {
    //   ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
    //   this->mark_failed();
    //   return;
    // }
    // esp_err_t err = this->socket_->bind((struct sockaddr *) &server, sizeof(server));
    // printf("err %d\n", err);

    // std::string hello_message = this->hello_message_serialize_();
    // ssize_t write_amount = this->socket_->write((void *) hello_message.front(), hello_message.length());

    // printf("write socket %d\n", write_amount);

    // struct sockaddr_in servaddr;
    ip_addr_t remote_ip;

    // servaddr.sin_family = AF_INET;
    // inet_pton(AF_INET, "192.168.1.35", &(servaddr.sin_addr.s_addr));
    // servaddr.sin_port = htons(1704);

    struct netconn *lwip_netconn;

    inet_pton(AF_INET, "192.168.1.35", &(remote_ip.addr));
    uint16_t remotePort = 1704;

    if (lwip_netconn != nullptr) {
      close_connection(lwip_netconn);
    }

    lwip_netconn = netconn_new(NETCONN_TCP);
    if (lwip_netconn == nullptr) {
      ESP_LOGE(TAG, "can't create netconn");
      continue;
    }

    ip_addr_t any_ip_addr = IPADDR4_INIT(IPADDR_ANY);

    esp_err_t err = ESP_OK;
    err = netconn_bind(lwip_netconn, &any_ip_addr, 0);
    if (err != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
      close_connection(lwip_netconn);
      continue;
    }

    err = netconn_connect(lwip_netconn, &remote_ip, remotePort);
    if (err != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d", ipaddr_ntoa(&remote_ip), remotePort, err);
      close_connection(lwip_netconn);
      continue;
    }

    int64_t now = esp_timer_get_time();

    std::string hello_msg = this_snapcast->hello_message_serialize_();

    size_t total_hello_msg_size = hello_msg.size() + sizeof(uint32_t);
    bytebuffer::ByteBuffer hello_msg_buffer = bytebuffer::ByteBuffer(total_hello_msg_size);

    hello_msg_buffer.put_uint32(total_hello_msg_size);
    for (size_t i = 0; i < hello_msg.size(); ++i) {
      hello_msg_buffer.put_uint8(hello_msg.data()[i]);
    }

    base_message base_msg = {
        .type = SNAPCAST_MESSAGE_HELLO,
        .id = 0x0000,
        .refersTo = 0x0000,
        .sent = {.sec = now / 1000000, .usec = now - now / 1000000},
        .received = {.sec = 0, .usec = 0},
        .size = total_hello_msg_size,
    };

    bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

    this_snapcast->base_message_serialize_(&base_msg, base_msg_buffer);

    printf("hello msg %s\n size with size: %d\n", hello_msg.c_str(), base_msg.size);
    printf("base message buffer size: %d, remaining: %d\n", base_msg_buffer.get_capacity(),
           base_msg_buffer.get_remaining());

    err = netconn_write(lwip_netconn, (void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE, NETCONN_NOCOPY);
    ESP_LOGD(TAG, "attempted to send base message,err %d", err);

    err = netconn_write(lwip_netconn, (void *) hello_msg_buffer.get_raw_data(), base_msg.size, NETCONN_NOCOPY);
    ESP_LOGD(TAG, "attempted to send hello message,err %d", err);

    struct netbuf *firstNetBuf = NULL;

    while (true) {
      err = netconn_recv(lwip_netconn, &firstNetBuf);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to receive message %d", err);

        close_connection(lwip_netconn);

        if (firstNetBuf != NULL) {
          netbuf_delete(firstNetBuf);
        }

        break;
      }

      netbuf_first(firstNetBuf);
      ESP_LOGD(TAG, "attempting to decode");

      uint8_t *start;
      uint16_t len;

      err = netbuf_data(firstNetBuf, (void **) &start, &len);
      if (err == ESP_OK) {
        printf("data len: %d, %d\n", len, netbuf_len(firstNetBuf));

        std::vector<uint8_t> base_msg_read_data;
        base_msg_read_data.insert(base_msg_read_data.end(), &start[0], &start[BASE_MESSAGE_SIZE]);
        printf("base message length: %d", base_msg_read_data.size());

        base_msg_buffer.reset();
        base_msg_buffer.put(base_msg_read_data);
        base_msg_buffer.rewind();
        this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);
        printf("base message response type: %d\n", base_msg.type);
        printf("base message size of next message: %d\n", base_msg.size);
        // base_msg.bytebuffer::ByteBuffer incoming_data = bytebuffer::ByteBuffer(len);

        switch (base_msg.type) {
          case SNAPCAST_MESSAGE_SERVER_SETTINGS:
            if (base_msg.type == SNAPCAST_MESSAGE_SERVER_SETTINGS) {
              bytebuffer::ByteBuffer server_settings_buffer = bytebuffer::ByteBuffer(base_msg.size);
              std::string server_msg_read_data = std::string(
                  (const char *) &start[BASE_MESSAGE_SIZE + sizeof(uint32_t)], base_msg.size - sizeof(uint32_t));
              // server_msg_read_data.insert(1, (const char *) &start[BASE_MESSAGE_SIZE], (size_t) base_msg.size);
              // server_settings_buffer.put(server_msg_read_data);
              // server_settings_buffer.rewind();
              server_settings_message server_settings_msg;
              this_snapcast->server_settings_message_deserialize_(&server_settings_msg, server_msg_read_data.c_str());

              printf(
                  "server settings json: %s\n server settings buffer ms: %d\n latency: %d\n muted: %d\n volume: %ds\n",
                  server_msg_read_data.c_str(), server_settings_msg.buffer_ms, server_settings_msg.latency,
                  server_settings_msg.muted, server_settings_msg.volume);

              // server_settings.copy_from(&base_msg.data[0], base_msg.data.size());
            }
            break;
          default:
            break;
        }
      }
      // break;
    }
    netbuf_delete(firstNetBuf);
    close_connection(lwip_netconn);
    while (true) {
      delay(10);
    }
  }
}

void SnapcastPlayer::base_message_serialize_(base_message_t *msg, bytebuffer::ByteBuffer &buffer) {
  buffer.put_uint16(msg->type);
  buffer.put_uint16(msg->id);
  buffer.put_uint16(msg->refersTo);
  buffer.put_int32(msg->sent.sec);
  buffer.put_int32(msg->sent.usec);
  buffer.put_int32(msg->received.sec);
  buffer.put_int32(msg->received.usec);
  buffer.put_uint32(msg->size);
}

void SnapcastPlayer::base_message_deserialize_(base_message_t *msg, bytebuffer::ByteBuffer &buffer) {
  msg->type = buffer.get_uint16();
  msg->id = buffer.get_uint16();
  msg->refersTo = buffer.get_uint16();
  msg->sent.sec = buffer.get_int32();
  msg->sent.usec = buffer.get_int32();
  msg->received.sec = buffer.get_int32();
  msg->received.usec = buffer.get_int32();
  msg->size = buffer.get_uint32();
}

std::string SnapcastPlayer::hello_message_serialize_() {
  char mac_address[18];
  uint8_t base_mac[6];
  esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
  sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4],
          base_mac[5]);
  hello_message_t hello_message;
  hello_message.mac = mac_address;
  hello_message.hostname = "my_hostname";
  hello_message.version = "0.0.1";
  hello_message.client_name = "libsnapcast";
  hello_message.os = "esp32";
  hello_message.arch = "xtensa";
  hello_message.instance = 1;
  hello_message.id = mac_address;
  hello_message.protocol_version = 2;
  return this->build_hello_message_(&hello_message);
}

std::string SnapcastPlayer::build_hello_message_(hello_message_t *msg) {
  return json::build_json([msg](JsonObject root) {
    root["MAC"] = msg->mac;
    root["HostName"] = msg->hostname;
    root["Version"] = msg->version;
    root["ClientName"] = msg->client_name;
    root["OS"] = msg->os;
    root["Arch"] = msg->arch;
    root["Instance"] = msg->instance;
    root["ID"] = msg->id;
    root["SnapStreamProtocolVersion"] = msg->protocol_version;
  });
}

bool SnapcastPlayer::server_settings_message_deserialize_(server_settings_message_t *msg, const char *json_str) {
  bool valid = json::parse_json(json_str, [msg](JsonObject root) -> bool {
    if (!root.containsKey("bufferMs") || !root.containsKey("latency") || !root.containsKey("muted") ||
        !root.containsKey("volume")) {
      ESP_LOGE(TAG, "Server settings message doesn't contain all the fields");
      return false;
    }
    msg->buffer_ms = root["bufferMs"].as<int32_t>();
    msg->latency = root["latency"].as<int32_t>();
    msg->volume = root["volume"].as<uint32_t>();
    msg->muted = root["muted"].as<uint32_t>();

    return true;
  });

  return valid;
}

}  // namespace snapcast
}  // namespace esphome
#endif
