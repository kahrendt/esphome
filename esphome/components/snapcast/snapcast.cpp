#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/log.h"

// #include "mdns.h"

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

void SnapcastPlayer::setup() {
  // // Find snapcast server
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

  this->socket_ = socket::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  struct sockaddr_storage server;

  socklen_t sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), "192.168.1.35", 1704);
  if (sl == 0) {
    ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
    this->mark_failed();
    return;
  }
  esp_err_t err = this->socket_->bind((struct sockaddr *) &server, sizeof(server));
  printf("err %d\n", err);

  std::string hello_message = this->hello_message_serialize_();
  ssize_t write_amount = this->socket_->write((void *) hello_message.front(), hello_message.length());

  printf("write socket %d\n", write_amount);
}
void SnapcastPlayer::loop() {}

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
  hello_message_t hello_message;
  hello_message.mac = "00:11:22:33:44:55";
  hello_message.hostname = "my_hostname";
  hello_message.version = "0.0.1";
  hello_message.client_name = "libsnapcast";
  hello_message.os = "esp32";
  hello_message.arch = "xtensa";
  hello_message.instance = 1;
  hello_message.id = "00:11:22:33:44:55";
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
