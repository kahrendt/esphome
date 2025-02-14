#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/audio/audio_decoder.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mdns.h"

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

static const uint32_t ENCODED_CHUNK_QUEUE_SIZE = 50;
static const uint32_t DECODED_CHUNK_QUEUE_SIZE = 50;

static const uint32_t FAST_SYNC_LATENCY_BUF = 10000;      // in µs
static const uint32_t NORMAL_SYNC_LATENCY_BUF = 1000000;  // in µs

void SnapcastPlayer::start() {
  this->speaker_->add_audio_output_callback([this](uint32_t frames_played, int64_t write_timestamp) {
    if (!this->chunk_timings_.empty()) {
      bool new_chunk = false;
      int32_t accumulated_chunk_corrections = 0;
      AudioSyncChunkTimings front_chunk = this->chunk_timings_.front();
      while (front_chunk.total_frames < frames_played) {
        frames_played -= front_chunk.total_frames;
        accumulated_chunk_corrections += front_chunk.frame_corrections;

        this->chunk_timings_.pop_front();
        front_chunk = this->chunk_timings_.front();

        new_chunk = true;
      }

      // Now we are in the middle of the current audio chunk

      int64_t full_precision_microseconds =
          (frames_played * 1000000LL) /
          static_cast<int64_t>(this->current_audio_stream_info_.value().get_sample_rate());
      int64_t server_timestamp_finished = front_chunk.server_timestamp + full_precision_microseconds;
      int64_t equivalent_client_timestamp = server_timestamp_finished -
                                            this->server_internal_clock_offset_.get_most_recent_median() +
                                            (this->snapcast_buffer_duration_ms_ - this->snapcast_latency_ms_) * 1000;
      this->chunk_timings_.front().total_frames -= frames_played;
      this->chunk_timings_.front().server_timestamp = server_timestamp_finished;

      if (abs(accumulated_chunk_corrections) > 10) {
        // Very large change, our median filter will be slow to a adapt
        this->actual_offsets_.reset();
      }
      this->pending_frame_corrections_ -= accumulated_chunk_corrections;

      int64_t internal_latency_written = front_chunk.internal_timestamp + full_precision_microseconds;
      // if (new_chunk) {
      int64_t new_error = equivalent_client_timestamp - write_timestamp;

      // xQueueSend(this->actual_offset_queue_, &new_error, 0);
      this->actual_offsets_.update(new_error);
      if (new_error > 22982976707978547LL) {
        printf("weirdly huge error. server timestamp should have been %" PRId64 "; client timestamp %" PRId64
               "; actually written %" PRId64 "\n",
               server_timestamp_finished, equivalent_client_timestamp, write_timestamp);
      }

      this->internal_latency_.update(internal_latency_written - write_timestamp);
      // }
    }
  });

  RAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->encoded_chunk_data_queue_storage_ = allocator.allocate(ENCODED_CHUNK_QUEUE_SIZE * sizeof(AudioSyncChunk));
  if (this->encoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->encoded_chunk_data_queue_ =
      xQueueCreateStatic(ENCODED_CHUNK_QUEUE_SIZE, sizeof(AudioSyncChunk), this->encoded_chunk_data_queue_storage_,
                         &encoded_chunk_data_queue_buffer_);

  this->decoded_chunk_data_queue_storage_ = allocator.allocate(DECODED_CHUNK_QUEUE_SIZE * sizeof(AudioSyncChunk));
  if (this->decoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }

  this->decoded_chunk_data_queue_ =
      xQueueCreateStatic(DECODED_CHUNK_QUEUE_SIZE, sizeof(AudioSyncChunk), this->decoded_chunk_data_queue_storage_,
                         &decoded_chunk_data_queue_buffer_);
  // this->encoded_chunk_data_queue_ = xQueueCreate(20, sizeof(AudioSyncChunk));
  xTaskCreate(snapcast_task, "snapcast", 1024 * 5, (void *) this, 5, &this->snapcast_task_handle_);
}

void SnapcastPlayer::loop() {}

void SnapcastPlayer::send_client_message() {
  ClientInfoMessage client_msg = {.volume = static_cast<uint32_t>(this->speaker_->get_volume() * 100.0f),
                                  .muted = this->external_mute_};
  std::string json_client_msg = this->client_message_serialize_(&client_msg);
  int64_t now = esp_timer_get_time();
  bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);
  BaseMessage base_msg_for_client_info = {
      .type = SNAPCAST_MESSAGE_CLIENT_INFO,
      .id = 0x0000,
      .refers_to = 0x0000,
      .sent = {.sec = static_cast<int32_t>(now / 1000000LL),
               .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
      .received = {.sec = 0, .usec = 0},
      .size = json_client_msg.size(),
  };
  this->base_message_serialize_(&base_msg_for_client_info, base_msg_buffer);
  this->socket_->write((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);
  this->socket_->write((void *) json_client_msg.data(), json_client_msg.size());

  printf("Sent the followign client message: %s\n", json_client_msg.c_str());
}

void SnapcastPlayer::time_sync_callback(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  bytebuffer::ByteBuffer time_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  int64_t now = esp_timer_get_time();
  BaseMessage base_msg_for_time = {
      .type = SNAPCAST_MESSAGE_TIME,
      .id = this_snapcast->time_sync_counter_++,
      .refers_to = 0x0000,
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
  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  while (true) {
    // Connect to first snapcast server found

    mdns_result_t *mdns_result;

    mdns_init();

    ESP_LOGI(TAG, "Lookup snapcast service on network");
    esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &mdns_result);

    if (!mdns_result) {
      ESP_LOGW(TAG, "No results found for snapcast service!");
    }

    char ip_address[16];
    bool use_mdns = false;
    uint16_t port = this_snapcast->server_port_;
    if (mdns_result->addr) {
      use_mdns = true;
      sprintf(ip_address, "%d.%d.%d.%d", IP2STR(mdns_result->addr));
      port = mdns_result->port;
      printf("found a snapcast server via mdns %s; ip is " IPSTR " sprintf ip %s\n", mdns_result->hostname,
             IP2STR(mdns_result->addr), ip_address);
    }
    mdns_query_results_free(mdns_result);

    this_snapcast->socket_ = socket::socket_ip(SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_storage server;

    socklen_t sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server),
                                        this_snapcast->server_address_.c_str(), this_snapcast->server_port_);
    if (use_mdns) {
      printf("socket actually set with mdns\n");
      sl = socket::set_sockaddr((struct sockaddr *) &server, sizeof(server), (const char *) ip_address, port);
    }

    if (sl == 0) {
      ESP_LOGE(TAG, "Socket unable to set sockaddr: errno %d", errno);
      continue;
    }
    err = this_snapcast->socket_->connect((struct sockaddr *) &server, sizeof(server));

    int nodelay = 1;
    if (this_snapcast->socket_->setsockopt(IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
      /* If failed to turn on TCP_NODELAY, throw warning and continue */
      printf("failed to turn on tcp_nodelay\n");
      nodelay = 0;
    }

    int64_t now = esp_timer_get_time();

    std::string hello_msg = this_snapcast->hello_message_serialize_();

    size_t total_hello_msg_size = hello_msg.size() + sizeof(uint32_t);
    bytebuffer::ByteBuffer hello_msg_buffer = bytebuffer::ByteBuffer(total_hello_msg_size);

    hello_msg_buffer.put_uint32(total_hello_msg_size);
    for (size_t i = 0; i < hello_msg.size(); ++i) {
      hello_msg_buffer.put_uint8(hello_msg.data()[i]);
    }

    BaseMessage base_msg = {
        .type = SNAPCAST_MESSAGE_HELLO,
        .id = 0x0000,
        .refers_to = 0x0000,
        .sent = {.sec = static_cast<int32_t>(now / 1000000),
                 .usec = static_cast<int32_t>(now - (now / 1000000LL) * 1000000LL)},
        .received = {.sec = 0, .usec = 0},
        .size = total_hello_msg_size,
    };

    bytebuffer::ByteBuffer base_msg_buffer = bytebuffer::ByteBuffer(BASE_MESSAGE_SIZE);

    this_snapcast->base_message_serialize_(&base_msg, base_msg_buffer);

    ssize_t write_amount = this_snapcast->socket_->write((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);

    write_amount = this_snapcast->socket_->write((void *) hello_msg_buffer.get_raw_data(), base_msg.size);

    printf("wrote hello message: %s\n", hello_msg.c_str());

    bool low_speed_timer_started = false;
    bool high_speed_timer_started = false;

    bool new_file_start = true;

    AudioSyncChunk *audio_chunk = chunk_allocator.allocate(1);

    if (audio_chunk == nullptr) {
      printf("failed to allocate audio chunk\n");
      continue;
    }

    int64_t last_time_sync_message = 0;
    uint32_t time_message_delay = FAST_SYNC_LATENCY_BUF;

    int64_t last_client_settings_message = 0;
    uint32_t client_message_delay = 60000000;

    while (true) {
      base_msg_buffer.rewind();

      ssize_t read_amount = this_snapcast->socket_->read((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);

      now = esp_timer_get_time();

      if (read_amount < BASE_MESSAGE_SIZE) {
        continue;
      }

      this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);

      base_msg.received.sec = static_cast<int32_t>(now / 1000000LL);
      base_msg.received.usec = static_cast<int32_t>(now - now / 1000000LL);

      if (high_speed_timer_started && this_snapcast->server_internal_clock_offset_.is_full()) {
        high_speed_timer_started = false;
        low_speed_timer_started = true;
        time_message_delay = NORMAL_SYNC_LATENCY_BUF;
      }

      switch (base_msg.type) {
        case SNAPCAST_MESSAGE_CODEC_HEADER: {
          printf("got snapcast header message\n");
          uint32_t codec_len = 0;
          this_snapcast->socket_->read(&codec_len, sizeof(uint32_t));

          std::string codec_type;
          codec_type.resize(codec_len);
          this_snapcast->socket_->read(&codec_type[0], codec_len);

          codec_len = 0;
          this_snapcast->socket_->read(&codec_len, sizeof(uint32_t));
          printf("codec format %s header has size %d bytes\n", codec_type.c_str(), codec_len);

          audio_chunk->server_timestamp = 0;
          audio_chunk->size = codec_len;
          audio_chunk->codec_header = true;
          if (codec_len > 0) {
            this_snapcast->speaker_->stop();

            new_file_start = true;
            xQueueReset(this_snapcast->encoded_chunk_data_queue_);
            xQueueReset(this_snapcast->decoded_chunk_data_queue_);

            size_t offset = 0;
            while (codec_len > 0) {
              ssize_t bytes_read = this_snapcast->socket_->read(audio_chunk->data + offset, codec_len);
              codec_len -= bytes_read;
              offset += bytes_read;
            }

            xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, portMAX_DELAY);

            if (this_snapcast->decode_task_handle_ == nullptr) {
              xTaskCreate(decode_task, "decode", 1024 * 5, (void *) this_snapcast, 1,
                          &this_snapcast->decode_task_handle_);
            }
          }

          if (!low_speed_timer_started) {
            time_message_delay = FAST_SYNC_LATENCY_BUF;
            high_speed_timer_started = true;
          }
          break;
        }
        case SNAPCAST_MESSAGE_WIRE_CHUNK: {
          bool valid_chunk = this_snapcast->current_audio_stream_info_.has_value();

          int32_t timestamp_s;
          int32_t timestamp_us;
          uint32_t chunk_size;
          this_snapcast->socket_->read(&timestamp_s, sizeof(timestamp_s));
          this_snapcast->socket_->read(&timestamp_us, sizeof(timestamp_us));
          this_snapcast->socket_->read(&chunk_size, sizeof(chunk_size));

          int64_t total_timestamp_us =
              static_cast<int64_t>(timestamp_s) * 1000000LL + static_cast<int64_t>(timestamp_us);

          audio_chunk->codec_header = false;
          audio_chunk->server_timestamp = total_timestamp_us;
          audio_chunk->size = chunk_size;
          if (chunk_size > 0) {
            if (chunk_size > MAX_CHUNK_SIZE) {
              valid_chunk = false;
              printf("got a wire chunk that had too big of a size\n");
            }
            size_t offset = 0;
            while (chunk_size > 0) {
              ssize_t actual_read_size = std::min(chunk_size, MAX_CHUNK_SIZE);
              ssize_t bytes_read = this_snapcast->socket_->read(audio_chunk->data + offset, actual_read_size);
              chunk_size -= bytes_read;
              if (valid_chunk) {
                offset += bytes_read;
              }
            }

            if (low_speed_timer_started && valid_chunk) {
              if (!xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, 0)) {
                printf("no room in data queue!\n");
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
            ServerSettingsMessage server_settings_msg;
            this_snapcast->server_settings_message_deserialize_(&server_settings_msg, server_msg_read_data.c_str());

            printf("server settings json: %s\n server settings buffer ms: %d\n latency: %d\n muted: %d\n volume %d\n",
                   server_msg_read_data.c_str(), server_settings_msg.buffer_ms, server_settings_msg.latency,
                   server_settings_msg.muted, server_settings_msg.volume);

            this_snapcast->snapcast_buffer_duration_ms_ = server_settings_msg.buffer_ms;
            if (server_settings_msg.latency != this_snapcast->snapcast_latency_ms_) {
              this_snapcast->actual_offsets_.reset();
            }
            this_snapcast->snapcast_latency_ms_ = server_settings_msg.latency;

            this_snapcast->speaker_->set_volume(static_cast<float>(server_settings_msg.volume) / 100.0f);
            this_snapcast->speaker_->set_mute_state(server_settings_msg.muted);
            this_snapcast->external_mute_ = server_settings_msg.muted;
          }

          break;
        }
        case SNAPCAST_MESSAGE_TIME: {
          int32_t latency_s;
          int32_t latency_us;
          this_snapcast->socket_->read(&latency_s, sizeof(latency_s));
          this_snapcast->socket_->read(&latency_us, sizeof(latency_us));

          int64_t time_rx_us = now;

          int64_t time_tx_us =
              static_cast<int64_t>(base_msg.sent.sec) * 1000000LL + static_cast<int64_t>(base_msg.sent.usec);
          int64_t t_dif = time_rx_us - time_tx_us;

          int64_t latency = static_cast<int64_t>(latency_s) * 1000000LL + static_cast<int64_t>(latency_us);

          int64_t tmp_dif = (latency - t_dif) / 2;

          int64_t server_clock_offset = this_snapcast->server_internal_clock_offset_.update(tmp_dif);

          // printf("median offset %" PRId64 " with server mismatch %" PRId64 " internal latency is %" PRId64
          //        "; pending frames to be corrected %d\n",
          //        server_clock_offset, this_snapcast->actual_offsets_.get_most_recent_median(),
          //        this_snapcast->internal_latency_.get_most_recent_median(),
          //        this_snapcast->pending_frame_corrections_);

          break;
        }

        default:

          break;
      }

      if (now - last_time_sync_message > time_message_delay) {
        this_snapcast->time_sync_callback(this_snapcast);
        last_time_sync_message = now;
      }
      // if (now - last_client_settings_message > client_message_delay) {
      //   this_snapcast->send_client_message();
      //   last_client_settings_message = now;
      // }
    }
    while (true) {
      delay(10);
    }
  }
}

void SnapcastPlayer::decode_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  AudioSyncChunk *encoded_chunk = chunk_allocator.allocate(1);
  AudioSyncChunk *decoded_chunk = chunk_allocator.allocate(1);
  std::unique_ptr<esp_audio_libs::flac::FLACDecoder> flac_decoder;

  while (true) {
    if (xQueueReceive(this_snapcast->encoded_chunk_data_queue_, encoded_chunk, pdMS_TO_TICKS(20))) {
      if (encoded_chunk->codec_header) {
        if (flac_decoder != nullptr) {
          flac_decoder.reset();
        }

        xQueueReset(this_snapcast->decoded_chunk_data_queue_);
        this_snapcast->actual_offsets_.reset();
        this_snapcast->pending_frame_corrections_ = 0;
        flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();
        auto result = flac_decoder->read_header(encoded_chunk->data, encoded_chunk->size);

        if (result == esp_audio_libs::flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
          printf("need more data\n");
          continue;
        }

        if (result != esp_audio_libs::flac::FLAC_DECODER_SUCCESS) {
          printf("serious error decodign flac header\n");
          continue;
        }

        size_t free_buffer_required = flac_decoder->get_output_buffer_size_bytes();

        this_snapcast->current_audio_stream_info_ = audio::AudioStreamInfo(
            flac_decoder->get_sample_depth(), flac_decoder->get_num_channels(), flac_decoder->get_sample_rate());

        printf("decoded flac header\n");

        if (this_snapcast->sync_task_handle_ == nullptr) {
          xTaskCreate(sync_task, "sync", 1024 * 3, (void *) this_snapcast, 1, &this_snapcast->sync_task_handle_);
        }

        this_snapcast->chunk_timings_.clear();
        this_snapcast->speaker_->set_audio_stream_info(this_snapcast->current_audio_stream_info_.value());

      } else if (flac_decoder != nullptr) {
        uint32_t output_samples = 0;
        auto result = flac_decoder->decode_frame(encoded_chunk->data, encoded_chunk->size,
                                                 reinterpret_cast<int16_t *>(decoded_chunk->data), &output_samples);

        if (result == esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
          printf("flac decoder ran out of a data\n");
          continue;
        }

        if (result > esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
          printf(" more serious flac decoder error\n");
          continue;
        }

        static bool logged_duration = false;

        size_t new_bytes = this_snapcast->current_audio_stream_info_.value().samples_to_bytes(output_samples);
        decoded_chunk->size = new_bytes;

        decoded_chunk->codec_header = false;
        decoded_chunk->server_timestamp = encoded_chunk->server_timestamp;
        xQueueSend(this_snapcast->decoded_chunk_data_queue_, decoded_chunk, pdMS_TO_TICKS(20));

        const uint32_t new_frames = this_snapcast->current_audio_stream_info_.value().bytes_to_frames(new_bytes);
        const uint32_t new_duration_us =
            this_snapcast->current_audio_stream_info_.value().frames_to_microseconds(new_frames);

        if (!logged_duration) {
          printf("new audio chunk has %d us duration\n", new_duration_us);
          logged_duration = true;
        }
      }
    }
  }
}

void SnapcastPlayer::sync_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  AudioSyncChunk *chunk = chunk_allocator.allocate(1);
  uint8_t chunks_since_last_adjustment = 25;
  while (true) {
    std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
        audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
    output_transfer_buffer->set_sink(this_snapcast->speaker_);
    bool run_once = false;
    uint8_t synced_chunks = 0;
    while (true) {
      output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));
      if (!xQueuePeek(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20))) {
        continue;
      }

      // if (chunk->size > output_transfer_buffer->free()) {
      //   continue;
      // }

      if (this_snapcast->speaker_->is_stopped()) {
        this_snapcast->speaker_->start();
      }

      if (!this_snapcast->chunk_timings_.empty()) {
        int64_t pending_frame_corrections = this_snapcast->pending_frame_corrections_;

        int64_t signed_pending_duration_corrections =
            (pending_frame_corrections * 1000000L) /
            static_cast<int64_t>(this_snapcast->current_audio_stream_info_.value().get_sample_rate());

        int64_t front_chunk_plays_at = this_snapcast->chunk_timings_.front().server_timestamp -
                                       this_snapcast->server_internal_clock_offset_.get_most_recent_median() +
                                       this_snapcast->snapcast_buffer_duration_ms_ * 1000 -
                                       this_snapcast->snapcast_latency_ms_ * 1000;
        int64_t us_to_start = front_chunk_plays_at - esp_timer_get_time();
        if (us_to_start - signed_pending_duration_corrections > 200000) {
          this_snapcast->speaker_->set_pause_state(true);
          uint32_t pause_time_ms = us_to_start / 2000;
          printf("chunk doesn't play for %" PRId64 "ms, so pausing for %d ms\n", us_to_start / 1000, pause_time_ms);
          vTaskDelay(pdMS_TO_TICKS(pause_time_ms));
          this_snapcast->speaker_->set_pause_state(false);
        }
      }

      int64_t pending_frame_corrections = this_snapcast->pending_frame_corrections_;

      int64_t signed_pending_duration_corrections =
          (pending_frame_corrections * 1000000L) /
          static_cast<int64_t>(this_snapcast->current_audio_stream_info_.value().get_sample_rate());

      int64_t recent_error_us = 0;

      if (this_snapcast->actual_offsets_.is_full()) {
        recent_error_us = this_snapcast->actual_offsets_.get_most_recent_median() - signed_pending_duration_corrections;
        if (abs(recent_error_us) < 5000) {
          synced_chunks = std::min(synced_chunks + 1, 10);
        } else {
          synced_chunks = 0;
        }
      }

      if ((synced_chunks < 10) && (!this_snapcast->speaker_->get_mute_state())) {
        printf("muting while waiting until we have a good sync");
        this_snapcast->speaker_->set_mute_state(true);
      } else if ((synced_chunks >= 10) &&
                 (this_snapcast->external_mute_ != this_snapcast->speaker_->get_mute_state())) {
        printf("sync is decent, setting to external mute state\n");
        this_snapcast->speaker_->set_mute_state(this_snapcast->external_mute_);
      }

      if (chunk->size > output_transfer_buffer->free()) {
        continue;
      } else {
        xQueueReceive(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20));
      }

      std::memcpy(output_transfer_buffer->get_buffer_end(), chunk->data, chunk->size);
      output_transfer_buffer->increase_buffer_length(chunk->size);

      uint32_t chunk_frame_count = this_snapcast->current_audio_stream_info_.value().bytes_to_frames(chunk->size);
      int32_t frame_corrections = 0;

      const size_t bytes_per_frame = this_snapcast->current_audio_stream_info_.value().frames_to_bytes(1);

      if (recent_error_us > 5000) {
        size_t silence_bytes =
            this_snapcast->current_audio_stream_info_.value().ms_to_bytes((recent_error_us - 2500) / 1000);
        size_t actual_silence_bytes = std::min(silence_bytes, output_transfer_buffer->free());
        std::memset((void *) (output_transfer_buffer->get_buffer_end() - chunk->size), 0,
                    actual_silence_bytes + chunk->size);
        output_transfer_buffer->increase_buffer_length(actual_silence_bytes);
        frame_corrections = this_snapcast->current_audio_stream_info_.value().bytes_to_frames(actual_silence_bytes);

        printf("adding %d frames of silence to hard sync, current error is %" PRId64
               "current pending %d us,current pending %d, internal current pending %d\n",
               frame_corrections, recent_error_us, signed_pending_duration_corrections,
               this_snapcast->pending_frame_corrections_, pending_frame_corrections);

      } else if (recent_error_us < -5000) {
        size_t bytes_to_remove =
            this_snapcast->current_audio_stream_info_.value().ms_to_bytes((abs(recent_error_us) - 2500) / 1000);
        size_t actual_bytes_to_remove = std::min(bytes_to_remove, chunk->size - bytes_per_frame);
        output_transfer_buffer->decrease_buffer_length(actual_bytes_to_remove);
        frame_corrections = -this_snapcast->current_audio_stream_info_.value().bytes_to_frames(actual_bytes_to_remove);
        printf("hard sync, removing %d frames from a chunk\n", frame_corrections);

      } else if (recent_error_us < -25) {
        const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
        int16_t *samples = reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
        for (int chan = 0; chan < num_channels; ++chan) {
          const int16_t left_sample = samples[chan];
          const int16_t right_sample = samples[num_channels + chan];
          samples[chan] = left_sample / 2 + right_sample / 2;
        }
        output_transfer_buffer->decrease_buffer_length(bytes_per_frame);
        frame_corrections = -1;
      } else if (recent_error_us > 25) {
        if (output_transfer_buffer->free() >= bytes_per_frame) {
          const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            const int16_t inserted_sample = left_sample / 2 + right_sample / 2;
            samples[num_channels + chan] = inserted_sample;
            samples[2 * num_channels + chan] = right_sample;
          }
          output_transfer_buffer->increase_buffer_length(bytes_per_frame);
          frame_corrections = 1;
        }
      }

      chunk_frame_count += frame_corrections;
      this_snapcast->pending_frame_corrections_ += frame_corrections;

      AudioSyncChunkTimings timings;
      timings.server_timestamp = chunk->server_timestamp;
      timings.internal_timestamp = esp_timer_get_time();
      timings.total_frames = chunk_frame_count;
      timings.frame_corrections = frame_corrections;
      this_snapcast->chunk_timings_.push_back(timings);
    }
  }
}

void SnapcastPlayer::base_message_serialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  buffer.put_uint16(msg->type);
  buffer.put_uint16(msg->id);
  buffer.put_uint16(msg->refers_to);
  buffer.put_int32(msg->sent.sec);
  buffer.put_int32(msg->sent.usec);
  buffer.put_int32(msg->received.sec);
  buffer.put_int32(msg->received.usec);
  buffer.put_uint32(msg->size);
}

void SnapcastPlayer::base_message_deserialize_(BaseMessage *msg, bytebuffer::ByteBuffer &buffer) {
  msg->type = buffer.get_uint16();
  msg->id = buffer.get_uint16();
  msg->refers_to = buffer.get_uint16();
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
  HelloMessage hello_message;
  hello_message.mac = mac_address;
  hello_message.hostname = App.get_name().c_str();
  hello_message.version = "0.0.1";
  hello_message.client_name = "esphome";
  hello_message.os = "esp32";
  hello_message.arch = "xtensa";
  hello_message.instance = 1;
  hello_message.id = mac_address;
  hello_message.protocol_version = 2;
  return this->build_hello_message_(&hello_message);
}

std::string SnapcastPlayer::client_message_serialize_(ClientInfoMessage *msg) {
  return json::build_json([msg](JsonObject root) {
    root["volume"] = msg->volume;
    root["muted"] = msg->muted;
  });
}

std::string SnapcastPlayer::build_hello_message_(HelloMessage *msg) {
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

bool SnapcastPlayer::server_settings_message_deserialize_(ServerSettingsMessage *msg, const char *json_str) {
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
