#include "snapcast.h"
#ifdef USE_NETWORK
// #include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/audio/audio_decoder.h"

#include "esphome/core/application.h"
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

static const uint32_t FAST_SYNC_LATENCY_BUF = 10000;      // in µs
static const uint32_t NORMAL_SYNC_LATENCY_BUF = 1000000;  // in µs

static void close_connection(struct netconn *conn) {
  if (conn != nullptr) {
    netconn_close(conn);
    netconn_delete(conn);
    conn = nullptr;
  }
}

void SnapcastPlayer::setup() {
  this->speaker_->add_audio_output_callback([this](uint32_t new_playback_ms, uint32_t remainder_us, uint32_t pending_ms,
                                                   uint32_t write_timestamp) {
    if (!this->chunk_timings_.empty()) {
      uint32_t frames_played = new_playback_ms;

      bool new_chunk = false;
      while (this->chunk_timings_.front().duration <= frames_played) {
        // if (this->first_audio_played_) {
        //   printf("chunk was")
        // }
        frames_played -= this->chunk_timings_.front().duration;
        this->chunk_timings_.pop_front();
        new_chunk = true;
      }

      // Now we are in the middle of the current audio chunk
      int64_t server_timestamp_finished =
          this->chunk_timings_.front().server_timestamp +
          this->current_audio_stream_info_.value().frames_to_microseconds(frames_played);
      int64_t equivalent_client_timestamp = server_timestamp_finished - this->previous_median_offset_ +
                                            (this->snapcast_buffer_duration_ms_ + this->snapcast_latency_ms_) * 1000;
      this->chunk_timings_.front().duration = this->chunk_timings_.front().duration - frames_played;
      if (new_chunk) {
        this->accumulated_drift_ = this->update_actual_offsets_(equivalent_client_timestamp - write_timestamp);
      }

      // this->accumulated_drift_ = equivalent_client_timestamp - write_timestamp;
      if (this->first_audio_played_) {
        this->first_audio_played_ = false;
        printf("supposed to write it at %" PRId64 "us (new calculation as this at %" PRId64
               "), actually finished writing out at %" PRId32 "us\n",
               this->initial_playback_timestamp_, equivalent_client_timestamp, write_timestamp);

        // int64_t error = (int64_t) (write_timestamp) - this->initial_playback_timestamp_;
        printf("that's an error of %" PRId64 "us\n", this->accumulated_drift_);
        // this->accumulated_drift_ += error;
      }
    }
    // if (!this->chunk_timings_.empty()) {
    //   uint32_t total_played_us = new_playback_ms * 1000 + remainder_us;

    //   if (this->first_audio_played_) {
    //     printf("total played us %d, initial chunk's duration is %d\n", total_played_us,
    //            this->chunk_timings_.front().duration);
    //   }

    //   bool new_chunk = false;
    //   while (this->chunk_timings_.front().duration < total_played_us) {
    //     // if (this->first_audio_played_) {
    //     //   printf("chunk was")
    //     // }
    //     total_played_us -= this->chunk_timings_.front().duration;
    //     this->chunk_timings_.pop_front();
    //     new_chunk = true;
    //   }

    //   // Now we are in the middle of the current audio chunk
    //   int64_t server_timestamp_finished = this->chunk_timings_.front().server_timestamp + total_played_us;
    //   int64_t equivalent_client_timestamp = server_timestamp_finished - this->previous_median_offset_ +
    //                                         (this->snapcast_buffer_duration_ms_ + this->snapcast_latency_ms_) * 1000;
    //   this->chunk_timings_.front().duration = this->chunk_timings_.front().duration - total_played_us;
    //   if (new_chunk) {
    //     this->accumulated_drift_ = this->update_actual_offsets_(equivalent_client_timestamp - write_timestamp);
    //   }

    //   // this->accumulated_drift_ = equivalent_client_timestamp - write_timestamp;
    //   if (this->first_audio_played_) {
    //     this->first_audio_played_ = false;
    //     printf("supposed to write it at %" PRId64 "us (new calculation as this at %" PRId64
    //            "), actually finished writing out at %" PRId32 "us\n",
    //            this->initial_playback_timestamp_, equivalent_client_timestamp, write_timestamp);

    //     // int64_t error = (int64_t) (write_timestamp) - this->initial_playback_timestamp_;
    //     printf("that's an error of %" PRId64 "us\n", this->accumulated_drift_);
    //     // this->accumulated_drift_ += error;
    //   }
    // }
  });

  RAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->encoded_chunk_data_queue_storage_ = allocator.allocate(20 * sizeof(AudioSyncChunk));
  if (this->encoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->encoded_chunk_data_queue_ = xQueueCreateStatic(
      20, sizeof(AudioSyncChunk), this->encoded_chunk_data_queue_storage_, &encoded_chunk_data_queue_buffer_);

  this->decoded_chunk_data_queue_storage_ = allocator.allocate(50 * sizeof(AudioSyncChunk));
  if (this->decoded_chunk_data_queue_storage_ == nullptr) {
    this->mark_failed();
    return;
  }

  this->decoded_chunk_data_queue_ = xQueueCreateStatic(
      50, sizeof(AudioSyncChunk), this->decoded_chunk_data_queue_storage_, &decoded_chunk_data_queue_buffer_);
  // this->encoded_chunk_data_queue_ = xQueueCreate(20, sizeof(AudioSyncChunk));
  xTaskCreate(snapcast_task, "snapcast", 1024 * 15, (void *) this, 1, &this->snapcast_task_handle_);
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
void SnapcastPlayer::unpause_callback(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  printf("unpausing at %" PRId64 "\n", esp_timer_get_time());
  this_snapcast->speaker_->set_pause_state(false);
}

void SnapcastPlayer::snapcast_task(void *params) {  // // Find snapcast server
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
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

    // std::unique_ptr<audio::AudioSinkTransferBuffer> transfer_buffer =
    //     audio::AudioSinkTransferBuffer::create(INPUT_BUFFER_SIZE);
    // {
    //   std::shared_ptr<RingBuffer> file_ring_buffer = RingBuffer::create(RING_BUFFER_SIZE);
    //   this_snapcast->raw_file_ring_buffer_ = file_ring_buffer;

    //   transfer_buffer->set_sink(this_snapcast->raw_file_ring_buffer_);
    // }

    esp_timer_create_args_t tSyncArgs = {.callback = &time_sync_callback,
                                         .arg = this_snapcast,
                                         .dispatch_method = ESP_TIMER_TASK,
                                         .name = "t_sync_msg",
                                         .skip_unhandled_events = false};
    esp_timer_handle_t timeSyncMessageTimer;
    esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

    esp_timer_create_args_t unpause_timer_args = {.callback = &unpause_callback,
                                                  .arg = this_snapcast,
                                                  .dispatch_method = ESP_TIMER_TASK,
                                                  .name = "t_unpause",
                                                  .skip_unhandled_events = false};
    esp_timer_handle_t unpauseTimer;
    esp_timer_create(&unpause_timer_args, &unpauseTimer);

    bool low_speed_timer_started = false;
    bool high_speed_timer_started = false;

    bool new_file_start = true;

    AudioSyncChunk *audio_chunk = chunk_allocator.allocate(1);

    if (audio_chunk == nullptr) {
      printf("failed to allocate audio chunk\n");
      continue;
    }

    while (true) {
      // transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(0));
      base_msg_buffer.rewind();

      ssize_t read_amount = this_snapcast->socket_->read((void *) base_msg_buffer.get_raw_data(), BASE_MESSAGE_SIZE);

      now = esp_timer_get_time();

      if (read_amount < BASE_MESSAGE_SIZE) {
        continue;
      }

      this_snapcast->base_message_deserialize_(&base_msg, base_msg_buffer);

      base_msg.received.sec = static_cast<int32_t>(now / 1000000LL);
      base_msg.received.usec = static_cast<int32_t>(now - now / 1000000LL);

      if (high_speed_timer_started && this_snapcast->time_offsets_.size() == 50) {
        if (esp_timer_is_active(timeSyncMessageTimer)) {
          esp_timer_stop(timeSyncMessageTimer);
        }
        high_speed_timer_started = false;
        esp_timer_start_periodic(timeSyncMessageTimer, NORMAL_SYNC_LATENCY_BUF);
        low_speed_timer_started = true;
        this_snapcast->accumulated_drift_ = 0;
      }

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

          codec_len = 0;
          this_snapcast->socket_->read(&codec_len, sizeof(uint32_t));
          printf("codec format %s header has size %d bytes\n", codec_type.c_str(), codec_len);

          audio_chunk->server_timestamp = 0;
          audio_chunk->size = codec_len;
          audio_chunk->codec_header = true;
          if (codec_len > 0) {
            new_file_start = true;
            xQueueReset(this_snapcast->encoded_chunk_data_queue_);

            size_t offset = 0;
            while (codec_len > 0) {
              ssize_t bytes_read = this_snapcast->socket_->read(audio_chunk->data + offset, codec_len);
              codec_len -= bytes_read;
              offset += bytes_read;
            }

            this_snapcast->speaker_->stop();
            xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, portMAX_DELAY);

            // this_snapcast->decoder_pause_ = true;
            xTaskCreate(decode_task, "decode", 1024 * 15, (void *) this_snapcast, 1,
                        &this_snapcast->decode_task_handle_);

            // this_snapcast->set_timeout(1500, [this_snapcast] { this_snapcast->decoder_pause_ = false; });
          }

          esp_timer_stop(timeSyncMessageTimer);
          if (!esp_timer_is_active(timeSyncMessageTimer)) {
            esp_timer_start_periodic(timeSyncMessageTimer, FAST_SYNC_LATENCY_BUF);
            high_speed_timer_started = true;
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

            if (low_speed_timer_started && new_file_start) {
              new_file_start = false;
              this_snapcast->speaker_->start();
              this_snapcast->speaker_->set_pause_state(true);

              printf("total_timestamp %" PRId64 "; median_offset %" PRId64 "; current time %" PRId64 "\n",
                     total_timestamp_us, this_snapcast->previous_median_offset_, esp_timer_get_time());

              this_snapcast->initial_playback_timestamp_ =
                  total_timestamp_us - this_snapcast->previous_median_offset_ + 1000000 + 27000;
              int64_t us_to_start = this_snapcast->initial_playback_timestamp_ - esp_timer_get_time();
              printf("initial playback in %" PRId64 "us\n", us_to_start);

              esp_timer_start_once(unpauseTimer, us_to_start);
              // this_snapcast->set_timeout("int_pause", (us_to_start / 1000),
              //                            [this_snapcast] { this_snapcast->speaker_->set_pause_state(false); });
            }

            audio_chunk->codec_header = false;
            audio_chunk->server_timestamp = total_timestamp_us;
            audio_chunk->size = chunk_size;
            if (chunk_size > 0) {
              size_t offset = 0;
              while (chunk_size > 0) {
                ssize_t bytes_read = this_snapcast->socket_->read(audio_chunk->data + offset, chunk_size);
                chunk_size -= bytes_read;
                offset += bytes_read;
              }

              if (low_speed_timer_started) {
                if (!xQueueSend(this_snapcast->encoded_chunk_data_queue_, audio_chunk, 0)) {
                  printf("no room in data queue!\n");
                }
              }

              // transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(0));
              // size_t bytes_to_read = std::min(transfer_buffer->free(), (size_t) chunk_size);
              // ssize_t bytes_read = this_snapcast->socket_->read(transfer_buffer->get_buffer_end(), bytes_to_read);
              // if (low_speed_timer_started) {
              //   // only use the data if we have had enough timer events ot detect the latency
              //   transfer_buffer->increase_buffer_length(bytes_read);
              // }
              // chunk_size -= bytes_read;
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

            this_snapcast->snapcast_buffer_duration_ms_ = server_settings_msg.buffer_ms;
            this_snapcast->snapcast_latency_ms_ = server_settings_msg.latency;

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

          // printf("time_rx_us=%" PRId64 ", time_tx_us=%" PRId64 ", t_dif=%" PRId64 ", latency=%" PRId64
          //        ", tmp_dif=%" PRId64 "\n",
          //        time_rx_us, time_tx_us, t_dif, latency, tmp_dif);

          int64_t server_mismatch_median = this_snapcast->update_time_offsets_(tmp_dif);

          printf("median offset %" PRId64 " with server mismatch %" PRId64 "\n", this_snapcast->previous_median_offset_,
                 server_mismatch_median);

          break;
        }

        default:
          // printf("received an unhandled snapcast message type: %d!\n", base_msg.type);
          // AudioSyncChunk audio_chunk;
          // size_t chunk_size = base_msg.size;
          // while (chunk_size > 0) {
          //   ssize_t bytes_read = this_snapcast->socket_->read(audio_chunk->data, std::min(chunk_size, (size_t)
          //   4096)); chunk_size -= bytes_read;
          // }

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

  RAMAllocator<AudioSyncChunk> chunk_allocator(ExternalRAMAllocator<AudioSyncChunk>::ALLOW_FAILURE);
  AudioSyncChunk *encoded_chunk = chunk_allocator.allocate(1);
  AudioSyncChunk *decoded_chunk = chunk_allocator.allocate(1);
  std::unique_ptr<esp_audio_libs::flac::FLACDecoder> flac_decoder;
  // std::unique_ptr<audio::AudioSinkTransferBuffer> transfer_buffer =
  //     audio::AudioSinkTransferBuffer::create(INPUT_BUFFER_SIZE);
  // transfer_buffer->set_sink(this_snapcast->speaker_);
  // uint8_t chunks_since_last_adjustment = 25;
  while (true) {
    if (xQueueReceive(this_snapcast->encoded_chunk_data_queue_, encoded_chunk, pdMS_TO_TICKS(20))) {
      // transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));

      if (encoded_chunk->codec_header) {
        flac_decoder.reset();
        flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();
        // if (!this_snapcast->current_audio_stream_info_.has_value()) {
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
        // if (transfer_buffer->capacity() < free_buffer_required) {
        // transfer_buffer->reallocate(free_buffer_required);
        // }

        this_snapcast->current_audio_stream_info_ = audio::AudioStreamInfo(
            flac_decoder->get_sample_depth(), flac_decoder->get_num_channels(), flac_decoder->get_sample_rate());

        printf("decoded flac header\n");

        // if (this_snapcast->sync_ring_buffer_.use_count() == 0) {
        //   const size_t ring_buffer_size = this_snapcast->current_audio_stream_info_.value().ms_to_bytes(
        //       (5 * this_snapcast->snapcast_buffer_duration_ms_) / 4);

        //   std::shared_ptr<RingBuffer> sync_ring_buffer = RingBuffer::create(ring_buffer_size);

        //   // transfer_buffer->set_sink(sync_ring_buffer);
        //   this_snapcast->sync_ring_buffer_ = sync_ring_buffer;

        if (this_snapcast->sync_task_handle_ == nullptr) {
          xTaskCreate(sync_task, "sync", 1024 * 15, (void *) this_snapcast, 1, &this_snapcast->sync_task_handle_);
        }
        // }

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
        // transfer_buffer->increase_buffer_length(new_bytes);

        // size_t bytes_per_frame = this_snapcast->current_audio_stream_info_.value().frames_to_bytes(1);
        // if ((chunks_since_last_adjustment > 20) && (new_bytes > 2 * bytes_per_frame)) {
        //   // if ((chunks_since_last_adjustment > 20) && (abs(this_snapcast->accumulated_drift_) < 5000)) {
        //   // Only add/remove sample if less than 5 ms off and we haven't adjusted in the last 20 chunks

        //   int32_t bytes_adjustment = 0;

        //   if (this_snapcast->accumulated_drift_ < -50) {
        //     this_snapcast->accumulated_drift_ += 21;

        //     // Drop a frame
        //     // new_bytes -= bytes_per_frame;

        //     const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
        //     // int16_t *samples = reinterpret_cast<int16_t *>(transfer_buffer->get_buffer_end() - 2 *
        //     bytes_per_frame); int16_t *samples = reinterpret_cast<int16_t *>(decoded_chunk->data + new_bytes - 2 *
        //     bytes_per_frame); for (int chan = 0; chan < num_channels; ++chan) {
        //       const int16_t left_sample = samples[chan];
        //       const int16_t right_sample = samples[num_channels + chan];
        //       samples[chan] = left_sample / 2 + right_sample / 2;
        //     }
        //     // transfer_buffer->decrease_buffer_length(bytes_per_frame);
        //     decoded_chunk->size -= bytes_per_frame;

        //   } else if (this_snapcast->accumulated_drift_ > 50) {
        //     this_snapcast->accumulated_drift_ -= 21;
        //     // if (transfer_buffer->free() >= bytes_per_frame) {
        //     // Insert a linearly inerpolated frame
        //     // new_bytes += bytes_per_frame;

        //     const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
        //     int16_t *samples = reinterpret_cast<int16_t *>(decoded_chunk->data + new_bytes - 2 * bytes_per_frame);
        //     for (int chan = 0; chan < num_channels; ++chan) {
        //       const int16_t left_sample = samples[chan];
        //       const int16_t right_sample = samples[num_channels + chan];
        //       const int16_t inserted_sample = left_sample / 2 + right_sample / 2;
        //       samples[num_channels + chan] = inserted_sample;
        //       samples[2 * num_channels + chan] = right_sample;
        //       // if (chan == 0) {
        //       //   printf("inserted a frame with value %" PRId16 " between %" PRId16 " and %" PRId16 "\n",
        //       //          samples[num_channels + chan], samples[chan], samples[2 * num_channels + chan]);
        //       // }
        //       decoded_chunk->size += bytes_per_frame;
        //     }
        //     // transfer_buffer->increase_buffer_length(bytes_per_frame);

        //     // }
        //   }
        //   chunks_since_last_adjustment = 0;
        // } else {
        //   ++chunks_since_last_adjustment;
        // }

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
        // AudioSyncChunkTimings timings;
        // timings.server_timestamp = encoded_chunk->server_timestamp;
        // // timings.duration = new_duration_us;
        // timings.duration = new_frames;
        // this_snapcast->chunk_timings_.push_back(timings);
        // We have successfully decoded some input data and have new output data
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
    // while (this_snapcast->sync_ring_buffer_.use_count() != 1) {
    //   delay(10);
    // }

    // std::unique_ptr<audio::AudioSourceTransferBuffer> input_transfer_buffer =
    //     audio::AudioSourceTransferBuffer::create(INPUT_BUFFER_SIZE);
    // input_transfer_buffer->set_source(this_snapcast->sync_ring_buffer_);

    std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
        audio::AudioSinkTransferBuffer::create(OUTPUT_BUFFER_SIZE);
    output_transfer_buffer->set_sink(this_snapcast->speaker_);

    while (true) {
      output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20));
      if (!xQueuePeek(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20))) {
        continue;
      }

      if (chunk->size > output_transfer_buffer->free()) {
        continue;
      } else {
        xQueueReceive(this_snapcast->decoded_chunk_data_queue_, chunk, pdMS_TO_TICKS(20));
      }

      // input_transfer_buffer->transfer_data_from_source(pdMS_TO_TICKS(20));

      // size_t bytes_to_transfer = std::min(chunk.size, output_transfer_buffer->free());
      std::memcpy(output_transfer_buffer->get_buffer_end(), chunk->data, chunk->size);

      // input_transfer_buffer->decrease_buffer_length(bytes_to_transfer);
      output_transfer_buffer->increase_buffer_length(chunk->size);

      int32_t bytes_adjustment = 0;
      const size_t bytes_per_frame = this_snapcast->current_audio_stream_info_.value().frames_to_bytes(1);
      if (chunks_since_last_adjustment > 25) {
        chunks_since_last_adjustment = 0;
        if (this_snapcast->accumulated_drift_ < -50) {
          this_snapcast->accumulated_drift_ += 21;
          const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            samples[chan] = left_sample / 2 + right_sample / 2;
          }
          // transfer_buffer->decrease_buffer_length(bytes_per_frame);
          chunk->size -= bytes_per_frame;

        } else if (this_snapcast->accumulated_drift_ > 50) {
          this_snapcast->accumulated_drift_ -= 21;
          const uint32_t num_channels = this_snapcast->current_audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            const int16_t inserted_sample = left_sample / 2 + right_sample / 2;
            samples[num_channels + chan] = inserted_sample;
            samples[2 * num_channels + chan] = right_sample;
            // if (chan == 0) {
            //   printf("inserted a frame with value %" PRId16 " between %" PRId16 " and %" PRId16 "\n",
            //          samples[num_channels + chan], samples[chan], samples[2 * num_channels + chan]);
            // }
            chunk->size += bytes_per_frame;
          }
        }
      } else {
        ++chunks_since_last_adjustment;
      }

      // transfer_buffer->increase_buffer_length(bytes_per_frame);

      // }

      AudioSyncChunkTimings timings;
      timings.server_timestamp = chunk->server_timestamp;
      // timings.duration = new_duration_us;
      timings.duration = this_snapcast->current_audio_stream_info_->bytes_to_frames(chunk->size);
      this_snapcast->chunk_timings_.push_back(timings);
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
  hello_message.hostname = App.get_name().c_str();
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
