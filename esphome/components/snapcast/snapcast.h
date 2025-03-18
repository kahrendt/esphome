#pragma once

#include "esphome/core/defines.h"

#ifdef USE_NETWORK

#include "median_filter.h"
#include "snapclient.h"
#include "snapcontrol.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "mdns.h"

#include <freertos/event_groups.h>
#include <freertos/queue.h>

#include <deque>

namespace esphome {
namespace snapcast {

// Stores the timing information of audio played received from the speaker
struct PlaybackProgress {
  uint32_t frames_played;    // Number of audio frames played since last progress update
  int64_t finish_timestamp;  // The timestamp when the audio frames should finish playing
};

// Stores the timing information for decoded chunks of audio sent to the speaker
struct InternalAudioTiming {
  int64_t server_timestamp;   // Server timestamp when this audio chunk should finish playing
  uint32_t total_frames;      // Total number of audio frames in this chunk, including corrections
  int32_t frame_corrections;  // Number of frames in this added/removed by the decoder to maintain sync
};

class SnapcastPlayer : public Component, public media_player::MediaPlayer {
 public:
  SnapcastPlayer() : actual_offsets_(MedianFilter(1)){};
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_CONNECTION; }
  // float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->is_muted_; }

  void start();

  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_server_address(std::string server_address) { this->server_address_ = std::move(server_address); }
  void set_server_port(uint16_t server_port) { this->server_port_ = server_port; }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  Trigger<bool, float> *get_server_settings_trigger() const { return this->server_settings_trigger_; }

  void publish_client_settings();

  void join_another_group();

 protected:
  // Receives commands from HA
  void control(const media_player::MediaPlayerCall &call) override;

  // Resets the encoded chunk queue after deallocating the data in each chunk.
  void clear_chunk_data_queue_();

  static void client_task(void *params);
  TaskHandle_t client_task_handle_{nullptr};
  StaticTask_t client_task_stack_;
  StackType_t *client_task_stack_buffer_{nullptr};

  static void control_task(void *params);
  TaskHandle_t control_task_handle_{nullptr};
  StaticTask_t control_task_stack_;
  StackType_t *control_task_stack_buffer_{nullptr};

  static void decode_task(void *params);
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};

  // Used to regularly send time messages to the snapserver for computing the total network latency
  static void timesync_callback(void *params);
  uint16_t time_sync_counter_{0};

  EventGroupHandle_t event_group_{nullptr};

  audio::AudioStreamInfo audio_stream_info_;

  optional<uint16_t> volume_;

  bool task_stack_in_psram_{false};

  bool is_muted_{false};

  bool force_publish_state_{false};

  std::string player_id_{""};

  speaker::Speaker *speaker_{nullptr};

  optional<std::string> server_address_;
  optional<std::string> discovered_address_;
  uint16_t server_port_;
  optional<uint16_t> server_control_port_;

  QueueHandle_t playback_progress_queue_;
  QueueHandle_t chunk_data_queue_;

  MedianFilter actual_offsets_;

  std::unique_ptr<Snapclient> snapclient_;
  std::unique_ptr<Snapcontrol> snapcontrol_;

  mdns_search_once_t *snapclient_mdns_search_{nullptr};
  mdns_search_once_t *snapcontrol_mdns_search_{nullptr};

  Trigger<bool, float> *server_settings_trigger_ = new Trigger<bool, float>();
};

template<typename... Ts> class PublishClientSettingsAction : public Action<Ts...>, public Parented<SnapcastPlayer> {
  void play(Ts... x) override { this->parent_->publish_client_settings(); }
};

}  // namespace snapcast
}  // namespace esphome
#endif
