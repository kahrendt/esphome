#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/audio/audio_decoder.h"

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

#include <flac_decoder.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

static const size_t INPUT_BUFFER_SIZE = 1024 * 50;
static const size_t OUTPUT_BUFFER_SIZE = 1024 * 50;
static const size_t RING_BUFFER_SIZE = 1024 * 1000;

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

void SnapcastPlayer::time_sync_callback(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  bytebuffer::ByteBuffer time_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  int64_t now = esp_timer_get_time();
  base_message base_msg_for_time = {
      .type = SNAPCAST_MESSAGE_TIME,
      .id = this_snapcast->time_sync_counter_++,
      .refersTo = 0x0000,
      .sent = {.sec = static_cast<int32_t>(now / 1000000LL),
               .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
      .received = {.sec = 0, .usec = 0},
      .size = TIME_MESSAGE_SIZE,
  };

  this_snapcast->base_message_serialize_(&base_msg_for_time, time_msg_buffer);
  time_msg_buffer.put_int32(0);
  time_msg_buffer.put_int32(0);
  this_snapcast->socket_->write((void *) time_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
}

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

    this_snapcast->socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_storage server;

    socklen_t sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), "192.168.1.35", 1704);
    if (sl == 0) {
      ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
      continue;
    }
    esp_err_t err = this_snapcast->socket_->connect((struct sockaddr *) &server, sizeof(server));
    printf("err %d\n", err);

    // std::string hello_message = this_snapcast->hello_message_serialize_();
    // ssize_t write_amount = this_snapcast->socket_->write((void *) hello_message.front(), hello_message.length());
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
        .sent = {.sec = static_cast<int32_t>(now / 1000000),
                 .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
        .received = {.sec = 0, .usec = 0},
        .size = total_hello_msg_size,
    };

    bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

    this_snapcast->base_message_serialize_(&base_msg, base_msg_buffer);

    printf("hello msg %s\n size with size: %d\n", hello_msg.c_str(), base_msg.size);
    printf("base message buffer size: %d, remaining: %d\n", base_msg_buffer.get_capacity(),
           base_msg_buffer.get_remaining());

    ssize_t write_amount = this_snapcast->socket_->write((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);

    printf("write socket base %d\n", write_amount);

    write_amount = this_snapcast->socket_->write((void *) hello_msg_buffer.get_raw_data(), base_msg.size);

    printf("write socket hello %d\n", write_amount);

    std::unique_ptr<audio::AudioSinkTransferBuffer> transfer_buffer =
        audio::AudioSinkTransferBuffer::create(INPUT_BUFFER_SIZE);
    {
      std::shared_ptr<RingBuffer> file_ring_buffer = RingBuffer::create(RING_BUFFER_SIZE);
      this_snapcast->raw_file_ring_buffer_ = file_ring_buffer;

      transfer_buffer->set_sink(this_snapcast->raw_file_ring_buffer_);
    }

    esp_timer_create_args_t tSyncArgs = {.callback = &time_sync_callback,
                                         .arg = this_snapcast,
                                         .dispatch_method = ESP_TIMER_TASK,
                                         .name = "t_sync_msg",
                                         .skip_unhandled_events = false};
    esp_timer_handle_t timeSyncMessageTimer;
    esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

    while (true) {
      transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(0));
      base_msg_buffer.rewind();

      ssize_t read_amount = this_snapcast->socket_->read((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);

      now = esp_timer_get_time();

      if (read_amount < BASE_MESSAGE_SIZE) {
        continue;
      }

      this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);

      base_msg.received.sec = static_cast<int32_t>(now / 1000000LL);
      base_msg.received.usec = static_cast<int32_t>(now - now / 1000000LL);

      // if (base_msg.type > 0) {
      //   printf("base message response type: %d\n", base_msg.type);
      //   printf("base message size of next message: %d\n", base_msg.size);
      // }

      switch (base_msg.type) {
        case SNAPCAST_MESSAGE_CODEC_HEADER: {
          printf("got snapcast header message\n");
          uint32_t codec_len = 0;
          this_snapcast->socket_->read(&codec_len, sizeof(uint32_t));

          std::string codec_type;
          codec_type.resize(codec_len);
          this_snapcast->socket_->read(&codec_type[0], codec_len);

          printf("codec format %s\n", codec_type.c_str());

          codec_len = 0;
          this_snapcast->socket_->read(&codec_len, sizeof(uint32_t));

          if (codec_len > 0) {
            while (codec_len > 0) {
              ssize_t bytes_read = this_snapcast->socket_->read(transfer_buffer->get_buffer_end(), codec_len);
              transfer_buffer->increase_buffer_length(bytes_read);
              codec_len -= bytes_read;
              printf("acutally read %d bytes \n", bytes_read);
            }

            xTaskCreate(decode_task, "decode", 1024 * 5, (void *) this_snapcast, 1,
                        &this_snapcast->decode_task_handle_);
          }

          esp_timer_stop(timeSyncMessageTimer);
          if (!esp_timer_is_active(timeSyncMessageTimer)) {
            esp_timer_start_periodic(timeSyncMessageTimer, 1000000);
          }
          break;
        }
        case SNAPCAST_MESSAGE_WIRE_CHUNK: {
          if (this_snapcast->current_audio_stream_info_.has_value()) {
            int32_t timestamp_s;
            int32_t timestamp_us;
            uint32_t chunk_size;
            this_snapcast->socket_->read(&timestamp_s, sizeof(timestamp_s));
            this_snapcast->socket_->read(&timestamp_us, sizeof(timestamp_us));
            this_snapcast->socket_->read(&chunk_size, sizeof(chunk_size));

            int64_t total_timestamp_us =
                static_cast<int64_t>(timestamp_s) * 1000000LL + static_cast<int64_t>(timestamp_us);
            // printf("play this chunk at %" PRId64 " (%" PRId32 "s, %" PRId32 "us); its currently %" PRId64 "\n",
            //  total_timestamp_us, timestamp_s, timestamp_us, esp_timer_get_time());

            if (chunk_size > 0) {
              while (chunk_size > 0) {
                transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(0));
                size_t bytes_to_read = std::min(transfer_buffer->free(), (size_t) chunk_size);
                ssize_t bytes_read = this_snapcast->socket_->read(transfer_buffer->get_buffer_end(), bytes_to_read);
                transfer_buffer->increase_buffer_length(bytes_read);
                chunk_size -= bytes_read;
              }
            }
          }
          break;
        }
        case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
          uint32_t server_settings_len = 0;
          this_snapcast->socket_->read(&server_settings_len, sizeof(uint32_t));
          if (server_settings_len > 0) {
            std::string server_msg_read_data;
            server_msg_read_data.resize(server_settings_len);
            this_snapcast->socket_->read(&server_msg_read_data[0], server_settings_len);
            server_settings_message server_settings_msg;
            this_snapcast->server_settings_message_deserialize_(&server_settings_msg, server_msg_read_data.c_str());

            printf("server settings json: %s\n server settings buffer ms: %d\n latency: %d\n muted: %d\n volume %d\n",
                   server_msg_read_data.c_str(), server_settings_msg.buffer_ms, server_settings_msg.latency,
                   server_settings_msg.muted, server_settings_msg.volume);

            this_snapcast->speaker_->set_volume(static_cast<float>(server_settings_msg.volume) / 100.0f);
            this_snapcast->speaker_->set_mute_state(server_settings_msg.muted);
          }

          break;
        }
        case SNAPCAST_MESSAGE_TIME: {
          int32_t latency_s;
          int32_t latency_us;
          this_snapcast->socket_->read(&latency_s, sizeof(latency_s));
          this_snapcast->socket_->read(&latency_us, sizeof(latency_us));

          int64_t time_rx_us = now;
          // int64_t time_rx_us =
          //     static_cast<int64_t>(base_msg.received.sec) * 1000000LL + static_cast<int64_t>(base_msg.received.usec);
          int64_t time_tx_us =
              static_cast<int64_t>(base_msg.sent.sec) * 1000000LL + static_cast<int64_t>(base_msg.sent.usec);
          int64_t t_dif = time_rx_us - time_tx_us;

          int64_t latency = static_cast<int64_t>(latency_s) * 1000000LL + static_cast<int64_t>(latency_us);

          int64_t tmp_dif = (latency - t_dif) / 2;

          printf("time_rx_us=%" PRId64 ", time_tx_us=%" PRId64 ", t_dif=%" PRId64 ", latency=%" PRId64
                 ", tmp_dif=%" PRId64 "\n",
                 time_rx_us, time_tx_us, t_dif, latency, tmp_dif);

          int64_t median_offset = this_snapcast->update_time_offsets_(tmp_dif);

          printf("media offset %" PRId64 "\n", median_offset);

          break;
        }

        default:
          break;
      }

      //   while (input_transfer_buffer->has_buffered_data() && stream_info.has_value() &&
      //          (output_transfer_buffer->free() >= free_buffer_required)) {
      //     uint32_t output_samples = 0;
      //     auto result = flac_decoder->decode_frame(
      //         input_transfer_buffer->get_buffer_start(), input_transfer_buffer->available(),
      //         reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), &output_samples);
      //     if (result == esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
      //       // Not an issue, just needs more data that we'll get next time.
      //       printf("ran out of data... \n");
      //       break;
      //     }

      //     size_t bytes_consumed = flac_decoder->get_bytes_index();
      //     input_transfer_buffer->decrease_buffer_length(bytes_consumed);

      //     if (result > esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
      //       // Corrupted frame, don't retry with current buffer content, wait for new sync
      //       printf("larger error\n");
      //       break;
      //     }

      //     // We have successfully decoded some input data and have new output data

      //     output_transfer_buffer->increase_buffer_length(stream_info.value().samples_to_bytes(output_samples));
      //     output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));
      //   }
      // }
    }
    while (true) {
      delay(10);
    }
  }
}

void SnapcastPlayer::decode_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  while (true) {
    std::unique_ptr<audio::AudioDecoder> decoder =
        make_unique<audio::AudioDecoder>(INPUT_BUFFER_SIZE, OUTPUT_BUFFER_SIZE);

    esp_err_t err = decoder->start(this_snapcast->current_audio_file_type_);

    if (this_snapcast->raw_file_ring_buffer_.use_count() > 0) {
      decoder->add_source(this_snapcast->raw_file_ring_buffer_);
    }

    if (err == ESP_OK) {
      bool has_stream_info = false;
      bool started_playback = false;

      printf("decoder task loop starting\n");
      while (true) {
        audio::AudioDecoderState decoder_state = decoder->decode(false);

        // if ((decoder_state == audio::AudioDecoderState::DECODING) ||
        //     (decoder_state == audio::AudioDecoderState::FINISHED)) {
        //   this_pipeline->playback_ms_ = decoder->get_playback_ms();
        // }

        // if (decoder_state == audio::AudioDecoderState::FINISHED) {
        //   break;
        // } else if (decoder_state == audio::AudioDecoderState::FAILED) {
        //   if (!has_stream_info) {
        //     event.decoding_err = DecodingError::FAILED_HEADER;
        //     xQueueSend(this_pipeline->info_error_queue_, &event, portMAX_DELAY);
        //   }
        //   xEventGroupSetBits(this_pipeline->event_group_,
        //                      EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
        //   break;
        // }

        if (!has_stream_info && decoder->get_audio_stream_info().has_value()) {
          has_stream_info = true;

          this_snapcast->current_audio_stream_info_ = decoder->get_audio_stream_info().value();

          // // Send the stream information to the pipeline
          // event.audio_stream_info = this_snapcast->current_audio_stream_info_;

          // if (this_pipeline->current_audio_stream_info_.get_bits_per_sample() != 16) {
          //   // Error state, incompatible bits per sample
          //   event.decoding_err = DecodingError::INCOMPATIBLE_BITS_PER_SAMPLE;
          //   xEventGroupSetBits(this_pipeline->event_group_,
          //                      EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
          // } else if ((this_pipeline->current_audio_stream_info_.get_channels() > 2)) {
          //   // Error state, incompatible number of channels
          //   event.decoding_err = DecodingError::INCOMPATIBLE_CHANNELS;
          //   xEventGroupSetBits(this_pipeline->event_group_,
          //                      EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
          // } else {
          // Send audio directly to the speaker
          this_snapcast->speaker_->set_audio_stream_info(this_snapcast->current_audio_stream_info_.value());
          decoder->add_sink(this_snapcast->speaker_);
          printf("got audio stream info");
          // }
        }
      }
    }
  }
  vTaskDelete(NULL);
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
  hello_message.hostname = "my_hostname4";
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
