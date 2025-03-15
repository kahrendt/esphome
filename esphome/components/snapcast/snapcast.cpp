#include "snapcast.h"

#ifdef USE_NETWORK
#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/components/network/util.h"
#include "esphome/components/json/json_util.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mdns.h"

#include <esp_timer.h>

#include <flac_decoder.h>
#include <opus.h>
#include <wav_decoder.h>

namespace esphome {
namespace snapcast {

static const char *TAG = "snapcast";

static const size_t INITIAL_BUFFER_SIZE =
    1024 * 10;  // Initial buffer size for the transfer buffer, will be reallocated to the minimum required size

static const uint32_t ENCODED_CHUNK_QUEUE_SIZE = 100;

static const uint32_t FAST_SYNC_LATENCY_BUF = 10000;      // in µs
static const uint32_t NORMAL_SYNC_LATENCY_BUF = 1000000;  // in µs

static const size_t CONTROL_TASK_STACK_SIZE = 3 * 1024;
static const size_t CLIENT_TASK_STACK_SIZE = 3 * 1024;
static const size_t DECODE_TASK_STACK_SIZE = 5 * 1024;

static const int GOOD_SYNCS_BEFORE_UNMUTE = 2;
static const int64_t HARD_SYNC_THRESHOLD_US = 5000;

static const UBaseType_t CLIENT_TASK_PRIORITY = 5;
static const UBaseType_t CONTROL_TASK_PRIORITY = 1;
static const UBaseType_t DECODE_TASK_PRIORITY = 1;

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),
  DECODE_FINISHED = (1 << 3),
  CONTROL_START = (1 << 7),
  WARNING_ENCODED_CHUNK_FULL = (1 << 11),
};

void SnapcastPlayer::setup() {
  this->player_id_ = get_mac_address_pretty();

  this->snapclient_ = make_unique<Snapclient>(this->player_id_);
  if (this->snapclient_ == nullptr) {
    ESP_LOGE(TAG, "Couldn't create snapclient object.");
    this->mark_failed();
  }

  this->snapcontrol_ = make_unique<Snapcontrol>(this->player_id_);
  if (this->snapcontrol_ == nullptr) {
    ESP_LOGE(TAG, "Couldn't create snapcontrol object.");
    this->mark_failed();
  }

  this->encoded_chunk_data_queue_ = xQueueCreate(ENCODED_CHUNK_QUEUE_SIZE, sizeof(AudioChunk));
  if (this->encoded_chunk_data_queue_ == nullptr) {
    ESP_LOGE(TAG, "Couldn't create encoded chunk data queue.");
    this->mark_failed();
  }

  this->playback_progress_queue_ = xQueueCreate(50, sizeof(PlaybackProgress));
  if (this->playback_progress_queue_ == nullptr) {
    ESP_LOGE(TAG, "Couldn't create playback progress queue.");
    this->mark_failed();
  }

  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Couldn't create event group.");
    this->mark_failed();
  }

  this->start();
}

void SnapcastPlayer::loop() {
  // if (network::is_connected() && !this->snapclient_->is_connected() && !this->server_address_.has_value() &&
  //     !this->discovered_address_.has_value()) {
  // // Search for a server
  // mdns_init();
  // mdns_result_t *mdns_result;

  // ESP_LOGD(TAG, "Looking for a snapcast service on network");

  // esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 50, 20, &mdns_result);

  // if (!mdns_result) {
  //   ESP_LOGW(TAG, "No results found for snapcast service!");
  // } else {
  //   if (mdns_result->addr) {
  //     network::IPAddress discovered_address = network::IPAddress(&mdns_result->addr->addr);
  //     this->discovered_address_ = discovered_address.str();
  //     this->server_port_ = mdns_result->port;
  //     ESP_LOGD(TAG, "Discovered a snapcast server via mdns: %s", discovered_address.str().c_str());
  //   }
  //   mdns_query_results_free(mdns_result);
  // }
  // }

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

  if ((event_bits & DECODE_FINISHED)) {
    // this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    if (this->speaker_->is_stopped()) {
      xQueueReset(this->playback_progress_queue_);
      xEventGroupClearBits(this->event_group_, (COMMAND_STOP | DECODE_FINISHED));
    } else {
      this->speaker_->stop();
    }
  }

  if (this->volume_.has_value()) {
    this->volume = static_cast<float>(this->volume_.value()) / 100.0f;
    this->speaker_->set_volume(this->volume);
    this->publish_client_settings();
    this->publish_state();
    this->volume_.reset();
  }

  if (this->snapcontrol_->get_stream_is_idle().has_value()) {
    if (this->snapcontrol_->get_stream_is_idle().value()) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      if ((old_state == media_player::MEDIA_PLAYER_STATE_PLAYING) && !this->speaker_->is_stopped()) {
        this->speaker_->finish();

        // Ensure we restore the proper mute state in case the stream was out of sync at the end
        this->speaker_->set_mute_state(this->is_muted_);
      }
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
    }
  }

  if ((this->state != old_state) || (this->force_publish_state_)) {
    this->force_publish_state_ = false;
    this->publish_state();
    ESP_LOGD(TAG, "State changed to %s", media_player::media_player_state_to_string(this->state));
  }
}

void SnapcastPlayer::start() {
  this->speaker_->add_audio_output_callback([this](uint32_t frames_played, int64_t write_timestamp) {
    PlaybackProgress playback_progress = {.frames_played = frames_played, .write_timestamp = write_timestamp};
    if (!xQueueSend(this->playback_progress_queue_, &playback_progress, 0)) {
      ESP_LOGE(TAG, "Playback info queue was full");
    }
  });

  if (this->client_task_stack_buffer_ == nullptr) {
    if (this->task_stack_in_psram_) {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
      this->client_task_stack_buffer_ = stack_allocator.allocate(CLIENT_TASK_STACK_SIZE);
    } else {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
      this->client_task_stack_buffer_ = stack_allocator.allocate(CLIENT_TASK_STACK_SIZE);
    }
  }

  if (this->control_task_stack_buffer_ == nullptr) {
    if (this->task_stack_in_psram_) {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
      this->control_task_stack_buffer_ = stack_allocator.allocate(CONTROL_TASK_STACK_SIZE);
    } else {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
      this->control_task_stack_buffer_ = stack_allocator.allocate(CONTROL_TASK_STACK_SIZE);
    }
  }

  if (this->decode_task_stack_buffer_ == nullptr) {
    if (this->task_stack_in_psram_) {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
      this->decode_task_stack_buffer_ = stack_allocator.allocate(DECODE_TASK_STACK_SIZE);
    } else {
      RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
      this->decode_task_stack_buffer_ = stack_allocator.allocate(DECODE_TASK_STACK_SIZE);
    }
  }

  this->client_task_handle_ =
      xTaskCreateStatic(client_task, "snap_client", CLIENT_TASK_STACK_SIZE, (void *) this, CLIENT_TASK_PRIORITY,
                        this->client_task_stack_buffer_, &this->client_task_stack_);
  this->control_task_handle_ =
      xTaskCreateStatic(control_task, "snap_control", CONTROL_TASK_STACK_SIZE, (void *) this, CONTROL_TASK_PRIORITY,
                        this->control_task_stack_buffer_, &this->control_task_stack_);
  this->decode_task_handle_ =
      xTaskCreateStatic(decode_task, "snap_decode", DECODE_TASK_STACK_SIZE, (void *) this, DECODE_TASK_PRIORITY,
                        this->decode_task_stack_buffer_, &this->decode_task_stack_);
}

void SnapcastPlayer::publish_client_settings() {
  if (!this->is_ready() || this->is_failed()) {
    // Ignore request before the media player is setup
    return;
  }

  this->snapclient_->send_client_message(this->speaker_->get_volume(), this->is_muted_);
}

void SnapcastPlayer::timesync_callback(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  this_snapcast->snapclient_->send_time_message();
}

media_player::MediaPlayerTraits SnapcastPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();

  traits.set_supports_pause(true);

  return traits;
}

void SnapcastPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!this->is_ready() || this->is_failed()) {
    // Ignore any commands sent before the media player is setup
    return;
  }

  if (call.get_volume().has_value()) {
    this->volume_ = round(call.get_volume().value() * 100.0f);
  }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:    // Intentional fallthrough
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:   // Intentional fallthrough
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:  // Intentional fallthrough
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        this->snapcontrol_->control_snapcast_stream(call.get_command().value());
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->speaker_->set_mute_state(true);
        this->is_muted_ = true;
        this->force_publish_state_ = true;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->speaker_->set_mute_state(false);
        this->is_muted_ = false;
        this->force_publish_state_ = true;
        break;
      default:
        break;
    }
  }
}

void SnapcastPlayer::control_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  bool first_attempt = true;

  while (!network::is_connected()) {
    delay(5000);
  }

  while (true) {
    if (this_snapcast->snapcontrol_->connect_to_server() != ESP_OK) {
      if (first_attempt) {
        ESP_LOGW(TAG, "Failed to connect to snapcontrol server, retrying silently in 5 seconds\n");
        first_attempt = false;
      }

      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    while (true) {
      this_snapcast->snapcontrol_->process_messages();
    }
  }
}

void SnapcastPlayer::clear_chunk_queue_() {
  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);
  AudioChunk chunk;
  while (xQueueReceive(this->encoded_chunk_data_queue_, &chunk, pdMS_TO_TICKS(1))) {
    data_allocator.deallocate(chunk.data, chunk.offset + chunk.size);
  }
}

void SnapcastPlayer::client_task(void *params) {  // // Find snapcast server
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;
  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);
  esp_timer_handle_t timesync_message_timer;
  static const esp_timer_create_args_t timer_for_syncing_args = {.callback = &timesync_callback,
                                                                 .arg = (void *) this_snapcast,
                                                                 .dispatch_method = ESP_TIMER_TASK,
                                                                 .name = "time_sync",
                                                                 .skip_unhandled_events = false};
  // create a timer to send time sync messages every x µs
  esp_timer_create(&timer_for_syncing_args, &timesync_message_timer);

  uint8_t failed_discover_count = 0;

  while (!network::is_connected()) {
    delay(5000);
  }

  while (true) {
    esp_timer_stop(timesync_message_timer);

    while (!this_snapcast->discovered_address_.has_value()) {
      // Search for a server
      mdns_init();
      mdns_result_t *mdns_result;

      ESP_LOGD(TAG, "Looking for a snapcast service on network");

      esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 1000, 20, &mdns_result);

      if (!mdns_result) {
        ESP_LOGW(TAG, "No results found for snapcast service!");
      } else {
        if (mdns_result->addr) {
          network::IPAddress discovered_address = network::IPAddress(&mdns_result->addr->addr);
          this_snapcast->discovered_address_ = discovered_address.str();
          this_snapcast->server_port_ = mdns_result->port;
          ESP_LOGD(TAG, "Discovered a snapcast server via mdns: %s", discovered_address.str().c_str());
        }
        mdns_query_results_free(mdns_result);
      }
    }

    if (this_snapcast->server_address_.has_value()) {
      if (this_snapcast->snapclient_->connect_to_server(this_snapcast->server_address_.value(),
                                                        this_snapcast->server_port_) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to connect to snapcast server configured in yaml, retrying in 5 seconds\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    } else if (this_snapcast->discovered_address_.has_value()) {
      if (this_snapcast->snapclient_->connect_to_server(this_snapcast->discovered_address_.value(),
                                                        this_snapcast->server_port_) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to connect to discovered snapcast server, retrying in 5 seconds\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        ++failed_discover_count;
        if (failed_discover_count >= 5) {
          // Repeatedly failed to connect to this server, search for a new one
          this_snapcast->discovered_address_.reset();
          failed_discover_count = 0;
        }
        continue;
      }
    } else {
      // Silently wait until a server is discovered
      delay(5);
      continue;
    }

    if (this_snapcast->snapclient_->send_hello_message() != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send the hello message, trying in 5 seconds.");
      this_snapcast->snapclient_->disconnect_from_server();
      continue;
    }

    bool low_speed_timer_started = false;
    bool high_speed_timer_started = false;

    int64_t last_time_sync_message = 0;
    uint32_t time_message_delay = FAST_SYNC_LATENCY_BUF;

    time_message_delay = FAST_SYNC_LATENCY_BUF;
    esp_timer_stop(timesync_message_timer);
    if (!esp_timer_is_active(timesync_message_timer)) {
      esp_timer_start_periodic(timesync_message_timer, time_message_delay);
    }
    high_speed_timer_started = true;

    while (true) {
      if (!this_snapcast->snapclient_->is_connected()) {
        break;
      }

      if (high_speed_timer_started && this_snapcast->snapclient_->get_network_latency_full()) {
        high_speed_timer_started = false;
        low_speed_timer_started = true;
        time_message_delay = NORMAL_SYNC_LATENCY_BUF;
        esp_timer_stop(timesync_message_timer);
        if (!esp_timer_is_active(timesync_message_timer)) {
          esp_timer_start_periodic(timesync_message_timer, time_message_delay);
        }
      }

      BaseMessage base_msg;
      if (this_snapcast->snapclient_->read_base_message(&base_msg) != ESP_OK) {
        this_snapcast->snapclient_->disconnect_from_server();
        break;
      }

      AudioChunk audio_chunk;
      audio_chunk.data = data_allocator.allocate(base_msg.size);

      if (audio_chunk.data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio chunk. Stopping playback and reconnecting to the server.");
        xEventGroupSetBits(this_snapcast->event_group_, COMMAND_STOP);
        this_snapcast->clear_chunk_queue_();
        this_snapcast->snapclient_->disconnect_from_server();
        break;
      }

      ProcessMessageResponse response = this_snapcast->snapclient_->process_messages(base_msg, &audio_chunk);
      switch (response) {
        case ProcessMessageResponse::PROCESSED_CODEC_HEADER: {
          // Stop decoding and clear any existing chunks in the queue
          xEventGroupSetBits(this_snapcast->event_group_, COMMAND_STOP);
          this_snapcast->clear_chunk_queue_();

          xQueueSend(this_snapcast->encoded_chunk_data_queue_, &audio_chunk, portMAX_DELAY);
          break;
        }
        case ProcessMessageResponse::PROCESSED_WIRE_CHUNK:
          if (!xQueueSend(this_snapcast->encoded_chunk_data_queue_, &audio_chunk, 0)) {
            ESP_LOGW(TAG, "Encoded chunk queue is full, dropping audio chunk.");
            data_allocator.deallocate(audio_chunk.data, audio_chunk.offset + audio_chunk.size);
            audio_chunk.data = nullptr;
          }
          break;
        case ProcessMessageResponse::PROCESSED_SERVER_SETTINGS: {
          ServerSettingsMessage server_settings = this_snapcast->snapclient_->get_server_settings_message();
          this_snapcast->defer([this_snapcast, server_settings]() {
            this_snapcast->server_settings_trigger_->trigger(server_settings.muted, server_settings.volume / 100.0f);
          });

          this_snapcast->volume_ = server_settings.volume;
          this_snapcast->speaker_->set_mute_state(server_settings.muted);
          this_snapcast->is_muted_ = server_settings.muted;

          data_allocator.deallocate(audio_chunk.data, audio_chunk.offset + audio_chunk.size);
          audio_chunk.data = nullptr;
          break;
        }
        case ProcessMessageResponse::PROCESSED_TIME:
          data_allocator.deallocate(audio_chunk.data, audio_chunk.offset + audio_chunk.size);
          audio_chunk.data = nullptr;
          break;
        case ProcessMessageResponse::ERROR_SOCKET_READ:
          data_allocator.deallocate(audio_chunk.data, audio_chunk.offset + audio_chunk.size);
          audio_chunk.data = nullptr;

          this_snapcast->snapclient_->disconnect_from_server();
          break;
        default:
          ESP_LOGE(TAG, "received an error from the snapclient");
          data_allocator.deallocate(audio_chunk.data, audio_chunk.offset + audio_chunk.size);
          audio_chunk.data = nullptr;
          break;
      }
    }
  }
  while (true) {
    delay(10);
  }
}

void SnapcastPlayer::decode_task(void *params) {
  SnapcastPlayer *this_snapcast = (SnapcastPlayer *) params;

  AudioChunk encoded_chunk;
  std::unique_ptr<esp_audio_libs::flac::FLACDecoder> flac_decoder;
  std::unique_ptr<esp_audio_libs::wav_decoder::WAVDecoder> wav_decoder;
  // std::unique_ptr<OpusDecoder> opus_decoder;
  OpusDecoder *opus_decoder = nullptr;
  size_t free_buffer_required = 8000;

  std::unique_ptr<audio::AudioSinkTransferBuffer> output_transfer_buffer =
      audio::AudioSinkTransferBuffer::create(INITIAL_BUFFER_SIZE);
  output_transfer_buffer->set_sink(this_snapcast->speaker_);

  int64_t pending_frame_corrections = 0;

  int synced_chunks = 0;

  std::deque<InternalAudioTiming> chunk_timings;

  RAMAllocator<uint8_t> data_allocator(RAMAllocator<uint8_t>::ALLOW_FAILURE);

  bool initial_decode = true;

  while (true) {
    EventBits_t event_bits = xEventGroupGetBits(this_snapcast->event_group_);
    if ((event_bits & COMMAND_STOP) && !(event_bits & DECODE_FINISHED)) {
      // this_snapcast->speaker_->stop();

      this_snapcast->audio_stream_info_.reset();
      this_snapcast->clear_chunk_queue_();

      // clear things we own as well
      output_transfer_buffer->decrease_buffer_length(output_transfer_buffer->available());
      // output_transfer_buffer.reset();
      // output_transfer_buffer = audio::AudioSinkTransferBuffer::create(INITIAL_BUFFER_SIZE);
      // output_transfer_buffer->set_sink(this_snapcast->speaker_);
      this_snapcast->actual_offsets_.reset();
      pending_frame_corrections = 0;
      chunk_timings.clear();

      xEventGroupSetBits(this_snapcast->event_group_, DECODE_FINISHED);
    }

    if (event_bits & DECODE_FINISHED) {
      delay(20);
      continue;
    }

    const size_t bytes_written = output_transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(20), false);

    if (output_transfer_buffer->available() > 0) {
      // Transfer buffer isn't empty, don't try to decode more
      continue;
    }

    size_t bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

    if (xQueuePeek(this_snapcast->encoded_chunk_data_queue_, &encoded_chunk, pdMS_TO_TICKS(20))) {
      bool receive_chunk = true;
      if (encoded_chunk.codec_header) {
        if (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_FLAC) {
          ESP_LOGD(TAG, "Decoding FLAC header");

          // Restart FLAC decoder
          flac_decoder.reset();
          flac_decoder = make_unique<esp_audio_libs::flac::FLACDecoder>();

          auto result = flac_decoder->read_header(encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size);

          if (result == esp_audio_libs::flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
            ESP_LOGW(TAG, "Need more data to decode FLAC header");
            continue;
          }

          if (result != esp_audio_libs::flac::FLAC_DECODER_SUCCESS) {
            ESP_LOGE(TAG, "Serious error decoding FLAC header");
            continue;
          }

          this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(
              flac_decoder->get_sample_depth(), flac_decoder->get_num_channels(), flac_decoder->get_sample_rate());

          bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

          free_buffer_required = flac_decoder->get_output_buffer_size_bytes();
          if (!output_transfer_buffer->reallocate(free_buffer_required + bytes_per_frame)) {
            ESP_LOGE(TAG, "Failed to reallocate buffer for decoding FLAC");
            continue;
          }
        } else if (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_PCM) {
          ESP_LOGD(TAG, "Decoding WAV header");

          // Restart the WAV decoder
          wav_decoder.reset();
          wav_decoder = make_unique<esp_audio_libs::wav_decoder::WAVDecoder>();
          wav_decoder->reset();

          esp_audio_libs::wav_decoder::WAVDecoderResult result =
              wav_decoder->decode_header(encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size);

          if (result == esp_audio_libs::wav_decoder::WAV_DECODER_SUCCESS_IN_DATA) {
            this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(
                wav_decoder->bits_per_sample(), wav_decoder->num_channels(), wav_decoder->sample_rate());
            bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);
          } else {
            ESP_LOGE(TAG, "Failed to parse WAV header");
            continue;
          }
        } else if (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_OPUS) {
          ESP_LOGD(TAG, "Decoding OPUS dummy header");

          uint32_t header_id;
          uint32_t sample_rate;
          uint16_t bit_depth;
          uint16_t channels;

          std::memcpy((void *) &header_id, (void *) (encoded_chunk.data + encoded_chunk.offset), sizeof(header_id));
          encoded_chunk.offset += sizeof(header_id);
          std::memcpy((void *) &sample_rate, (void *) (encoded_chunk.data + encoded_chunk.offset), sizeof(sample_rate));
          encoded_chunk.offset += sizeof(sample_rate);
          std::memcpy((void *) &bit_depth, (void *) (encoded_chunk.data + encoded_chunk.offset), sizeof(bit_depth));
          encoded_chunk.offset += sizeof(bit_depth);
          std::memcpy((void *) &channels, (void *) (encoded_chunk.data + encoded_chunk.offset), sizeof(channels));

          printf("sample_rate =%d, bit_depth =%d, channels=%d\n", sample_rate, bit_depth, channels);
          size_t decoder_size = opus_decoder_get_size(channels);
          printf("dedocder size needed for opus %d\n", decoder_size);

          opus_decoder = (OpusDecoder *) data_allocator.allocate(decoder_size);
          int decoder_error = opus_decoder_init(opus_decoder, sample_rate, channels);

          // int decoder_error;
          // opus_decoder = opus_decoder_create(sample_rate, channels, &decoder_error);

          if (decoder_error == OPUS_OK) {
            this_snapcast->audio_stream_info_ = audio::AudioStreamInfo(bit_depth, channels, sample_rate);
            bytes_per_frame = this_snapcast->audio_stream_info_.value().frames_to_bytes(1);

            free_buffer_required = this_snapcast->audio_stream_info_.value().ms_to_bytes(120);
            if (!output_transfer_buffer->reallocate(free_buffer_required + bytes_per_frame)) {
              ESP_LOGE(TAG, "Failed to reallocate buffer for decoding OPUS");
              continue;
            }
          } else {
            ESP_LOGE(TAG, "Failed to create OPUS decoder, error %d", decoder_error);
            continue;
          }
        }

        this_snapcast->actual_offsets_.reset();
        pending_frame_corrections = 0;
        chunk_timings.clear();
        xQueueReset(this_snapcast->playback_progress_queue_);

        this_snapcast->speaker_->set_audio_stream_info(this_snapcast->audio_stream_info_.value());
        initial_decode = true;
      } else {
        /** Use the information from the speaker on frames played to update teh current error */

        PlaybackProgress playback_progress;
        while (xQueueReceive(this_snapcast->playback_progress_queue_, &playback_progress, 0) == pdTRUE) {
          if (initial_decode) {
            // Some sent audio chunks have now been played by the speaker
            initial_decode = false;
            this_snapcast->snapcontrol_->control_get_server_status();  // Determine what stream this client is playing
          }

          if (!chunk_timings.empty()) {
            uint32_t frames_played = playback_progress.frames_played;
            int64_t write_timestamp = playback_progress.write_timestamp;

            InternalAudioTiming *front_chunk = &chunk_timings.front();

            pending_frame_corrections -= front_chunk->frame_corrections;
            front_chunk->frame_corrections = 0;

            while (front_chunk->total_frames < frames_played) {
              frames_played -= front_chunk->total_frames;

              chunk_timings.pop_front();
              front_chunk = &chunk_timings.front();

              pending_frame_corrections -= front_chunk->frame_corrections;
              front_chunk->frame_corrections = 0;
            }

            // Now we are in the middle of the current audio chunk

            chunk_timings.front().total_frames -= frames_played;

            uint32_t unplayed_frames = chunk_timings.front().total_frames;

            int64_t unplayed_ms =
                this_snapcast->audio_stream_info_.value().frames_to_milliseconds_with_remainder(&unplayed_frames);
            int64_t unplayed_us =
                1000 * unplayed_ms + this_snapcast->audio_stream_info_.value().frames_to_microseconds(unplayed_frames);

            int64_t server_timestamp_finished = front_chunk->server_timestamp - unplayed_us;
            int64_t equivalent_client_timestamp =
                this_snapcast->snapclient_->server_timestamp_to_client(server_timestamp_finished);

            int64_t new_error = equivalent_client_timestamp - write_timestamp;

            this_snapcast->actual_offsets_.update(new_error);
          }
        }

        /*******************/
        /*****Determine teh current error with pending correction */

        int64_t signed_pending_duration_corrections =
            (pending_frame_corrections * 1000000LL) /
            static_cast<int64_t>(this_snapcast->audio_stream_info_.value().get_sample_rate());

        // Takes into account the pending error
        int64_t recent_error_us =
            this_snapcast->actual_offsets_.get_most_recent_median() - signed_pending_duration_corrections;

        if (abs(this_snapcast->actual_offsets_.get_most_recent_median()) < HARD_SYNC_THRESHOLD_US) {
          synced_chunks = std::min(synced_chunks + 1, GOOD_SYNCS_BEFORE_UNMUTE);
        } else if (recent_error_us > HARD_SYNC_THRESHOLD_US) {
          // Even with the upcoming adjustments we are out of sync, reset the count
          synced_chunks = 0;
        }

        // Only mute/unmute for out of sync if we are receiving audio data
        if ((synced_chunks < GOOD_SYNCS_BEFORE_UNMUTE) && (!this_snapcast->speaker_->get_mute_state())) {
          ESP_LOGD(TAG, "Out of sync, muting output until corrected");
          this_snapcast->speaker_->set_mute_state(true);
        } else if ((synced_chunks >= GOOD_SYNCS_BEFORE_UNMUTE) &&
                   (this_snapcast->is_muted_ != this_snapcast->speaker_->get_mute_state())) {
          ESP_LOGD(TAG, "In sync with server, setting mute state to existing setting");
          this_snapcast->speaker_->set_mute_state(this_snapcast->is_muted_);
        }

        size_t new_bytes = 0;
        if ((flac_decoder != nullptr) &&
            (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_FLAC)) {
          uint32_t output_samples = 0;
          auto result = flac_decoder->decode_frame(
              encoded_chunk.data + encoded_chunk.offset, encoded_chunk.size,
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end()), &output_samples);

          if (result == esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
            ESP_LOGE(TAG, "FLAC decoder ran out of data");
            continue;
          }

          if (result > esp_audio_libs::flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
            ESP_LOGE(TAG, "Serious error decoding FLAC file");
            continue;
          }
          new_bytes = this_snapcast->audio_stream_info_.value().samples_to_bytes(output_samples);
        } else if ((wav_decoder != nullptr) &&
                   (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_PCM)) {
          if (output_transfer_buffer->capacity() < encoded_chunk.size + bytes_per_frame) {
            if (!output_transfer_buffer->reallocate(encoded_chunk.size + bytes_per_frame)) {
              ESP_LOGE(TAG, "Failed to reallocate buffer for the PCM audio chunk");
              continue;
            }
          }
          std::memcpy((void *) output_transfer_buffer->get_buffer_end(),
                      (void *) (encoded_chunk.data + encoded_chunk.offset), encoded_chunk.size);
          new_bytes = encoded_chunk.size;
        } else if ((opus_decoder != nullptr) &&
                   (this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_OPUS)) {
          int output_frames =
              opus_decode(opus_decoder, (encoded_chunk.data + encoded_chunk.offset), encoded_chunk.size,
                          (int16_t *) output_transfer_buffer->get_buffer_end(),
                          this_snapcast->audio_stream_info_.value().bytes_to_frames(output_transfer_buffer->free()), 0);
          if (output_frames < 0) {
            printf("ran into an issue decoding opus error code %d\n", output_frames);
          } else {
            new_bytes = this_snapcast->audio_stream_info_.value().frames_to_bytes(output_frames);
          }
        }

        output_transfer_buffer->increase_buffer_length(new_bytes);

        uint32_t new_frames = this_snapcast->audio_stream_info_.value().bytes_to_frames(new_bytes);
        const uint32_t new_duration_ms =
            this_snapcast->audio_stream_info_.value().frames_to_milliseconds_with_remainder(&new_frames);
        const int64_t new_duration_us =
            new_duration_ms * 1000 + this_snapcast->audio_stream_info_.value().frames_to_microseconds(new_frames);

        // How many frames in this chunk that are added or removed to the actual frame count
        int32_t frame_corrections = 0;

        const int64_t us_per_frame_margin = 3 * this_snapcast->audio_stream_info_.value().frames_to_microseconds(1) / 2;

        if (initial_decode || (recent_error_us > HARD_SYNC_THRESHOLD_US)) {
          // // Hard sync because we just started decoding and haven't sent any audio or we are way behind
          // // Zero out all new audio data and any extra bytes free in the transfer buffer
          // size_t silence_bytes = this_snapcast->audio_stream_info_.value().ms_to_bytes(recent_error_us / 1000);
          // size_t actual_silence_bytes = std::min(silence_bytes, output_transfer_buffer->free());
          // std::memset((void *) (output_transfer_buffer->get_buffer_end() - new_bytes), 0,
          //             actual_silence_bytes + new_bytes);
          // output_transfer_buffer->increase_buffer_length(actual_silence_bytes);
          // frame_corrections = this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_silence_bytes);

          // Zero out this chunks audio and any remaining free bytes in the transfer buffer
          const size_t zeroed_bytes = new_bytes + output_transfer_buffer->free();
          std::memset((void *) (output_transfer_buffer->get_buffer_end() - new_bytes), 0, zeroed_bytes);
          output_transfer_buffer->decrease_buffer_length(new_bytes);  // Remove the newly added length

          const size_t silence_bytes_for_correction =
              this_snapcast->audio_stream_info_.value().ms_to_bytes(recent_error_us / 1000);
          size_t actual_bytes_of_silence = std::min(silence_bytes_for_correction, zeroed_bytes);
          if (initial_decode) {
            actual_bytes_of_silence = zeroed_bytes;
          }
          output_transfer_buffer->increase_buffer_length(actual_bytes_of_silence);
          frame_corrections = this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_bytes_of_silence);

          ESP_LOGD(TAG,
                   "Hard sync: adding %" PRId32 " frames of silence. Current error is %" PRId64 "us. There are %" PRId64
                   "pending frames for correction",
                   frame_corrections, recent_error_us, pending_frame_corrections);
          receive_chunk = false;  // Don't actually process this frame since we didn't use any data from it
        } else if (recent_error_us < -HARD_SYNC_THRESHOLD_US) {
          // Hard sync because we have gotten ahead and need to skip some audio to get in sync
          // Removes newly decoded frames (but will always leave a minimum of 1 frame)
          size_t bytes_to_remove = this_snapcast->audio_stream_info_.value().ms_to_bytes(abs(recent_error_us) / 1000);
          size_t actual_bytes_to_remove = std::min(bytes_to_remove, new_bytes - bytes_per_frame);
          output_transfer_buffer->decrease_buffer_length(actual_bytes_to_remove);
          frame_corrections = -this_snapcast->audio_stream_info_.value().bytes_to_frames(actual_bytes_to_remove);
          ESP_LOGD(TAG,
                   "Hard sync: removing %" PRId32 " frames. Current error is %" PRId64 "us. There are %" PRId64
                   "pending frames for correction",
                   frame_corrections, recent_error_us, pending_frame_corrections);
        } else if (recent_error_us < -us_per_frame_margin) {
          // Small sync adjustment after getting slightly ahead.
          // Removes the last frame in the chunk to get in sync. The second to last frame is replaced with the average
          // of it and the removed frame to minimize audible glitches.
          const uint32_t num_channels = this_snapcast->audio_stream_info_.value().get_channels();
          int16_t *samples =
              reinterpret_cast<int16_t *>(output_transfer_buffer->get_buffer_end() - 2 * bytes_per_frame);
          for (int chan = 0; chan < num_channels; ++chan) {
            const int16_t left_sample = samples[chan];
            const int16_t right_sample = samples[num_channels + chan];
            samples[chan] = left_sample / 2 + right_sample / 2;
          }
          output_transfer_buffer->decrease_buffer_length(bytes_per_frame);
          frame_corrections = -1;
        } else if (recent_error_us > us_per_frame_margin) {
          // Small sync adjustment after getting slightly behind.
          // Adds one new frame to get in sync. The new frame is inserted between the last and second to last frames.
          // The new frame is the average of the last two frames in the chunk to minimize audible glitches.
          if (output_transfer_buffer->free() >= bytes_per_frame) {
            const uint32_t num_channels = this_snapcast->audio_stream_info_.value().get_channels();
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

        InternalAudioTiming timings;

        if (receive_chunk) {
          timings.server_timestamp = encoded_chunk.server_timestamp + new_duration_us;
          timings.total_frames =
              this_snapcast->audio_stream_info_.value().bytes_to_frames(new_bytes) + frame_corrections;
          timings.frame_corrections = frame_corrections;
          pending_frame_corrections += frame_corrections;
        } else {
          timings.server_timestamp = encoded_chunk.server_timestamp;
          timings.total_frames = frame_corrections;
          timings.frame_corrections = frame_corrections;
          pending_frame_corrections += frame_corrections;
        }

        chunk_timings.push_back(timings);

        // static int log_count = 0;
        // ++log_count;
        // if (log_count > 50) {
        //   printf("Current sync error: %" PRId64 "\n", this_snapcast->actual_offsets_.get_most_recent_median());
        //   log_count = 0;
        // }
      }
      if (receive_chunk ||
          this_snapcast->snapclient_->get_codec_format() == SnapcastCodecFormat::SNAPCAST_CODEC_UNSUPPORTED) {
        // Pop the chunk off the queue if we actually sent the audio or if it is in an unsupported format
        xQueueReceive(this_snapcast->encoded_chunk_data_queue_, &encoded_chunk, portMAX_DELAY);
        data_allocator.deallocate(encoded_chunk.data, encoded_chunk.offset + encoded_chunk.size);
      }
    }
    static uint32_t high_water_mark = 8192;
    uint32_t new_high_water_mark = uxTaskGetStackHighWaterMark(nullptr);
    if (new_high_water_mark < high_water_mark) {
      ESP_LOGD(TAG, "Decode task - High water mark changed from %d to %d.", high_water_mark, new_high_water_mark);
      high_water_mark = new_high_water_mark;
    }
  }
}

}  // namespace snapcast
}  // namespace esphome
#endif
