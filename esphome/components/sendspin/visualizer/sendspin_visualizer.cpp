#include "sendspin_visualizer.h"

#ifdef USE_ESP32

#include "../sendspin_hub.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <esp_timer.h>

namespace esphome {
namespace sendspin {

static const char *const TAG = "sendspin.visualizer";

// Visualizer binary message header: [type(1)] [num_frames(1)]
static const size_t VISUALIZER_BINARY_HEADER_SIZE = 2;
// Each frame starts with an 8-byte timestamp
static const size_t FRAME_TIMESTAMP_SIZE = 8;

void SendspinVisualizer::setup() {
  // Allocate spectrum buffer if spectrum type is requested
  if (this->spectrum_config_.has_value()) {
    this->configured_bin_count_ = this->spectrum_config_.value().n_disp_bins;
    this->spectrum_buffer_ = std::make_unique<uint16_t[]>(this->configured_bin_count_);
    std::memset(this->spectrum_buffer_.get(), 0, this->configured_bin_count_ * sizeof(uint16_t));
    // Point the base class to our buffer
    this->spectrum_bins_ = this->spectrum_buffer_.get();
    this->spectrum_bin_count_ = this->configured_bin_count_;
  }

  // Compute frame slot size and derive ring buffer capacity from buffer_capacity.
  // Wire frame size = 8 (timestamp) + data bytes per frame.
  // We compute the data size from configured types (known at config time).
  this->frame_slot_size_ = sizeof(VisualizerFrame) + this->configured_bin_count_ * sizeof(uint16_t);
  size_t wire_frame_size = FRAME_TIMESTAMP_SIZE;
  for (const auto &type : this->requested_types_) {
    switch (type) {
      case VisualizerDataType::LOUDNESS:
        wire_frame_size += 2;
        break;
      case VisualizerDataType::F_PEAK:
        wire_frame_size += 2;
        break;
      case VisualizerDataType::SPECTRUM:
        wire_frame_size += this->configured_bin_count_ * 2;
        break;
      default:
        break;
    }
  }
  this->ring_capacity_ = std::max(size_t{16}, this->buffer_capacity_ / wire_frame_size);
  this->frame_ring_ = std::make_unique<uint8_t[]>(this->ring_capacity_ * this->frame_slot_size_);
  std::memset(this->frame_ring_.get(), 0, this->ring_capacity_ * this->frame_slot_size_);

  // Register support object and callbacks with hub
  this->parent_->set_visualizer_support(this->get_support_object());
  this->parent_->set_visualizer_data_callback(
      [this](const uint8_t *data, size_t length) { this->on_visualizer_data(data, length); });
  this->parent_->set_beat_data_callback(
      [this](const uint8_t *data, size_t length) { this->on_beat_data(data, length); });
  this->parent_->set_visualizer_stream_callbacks(
      [this](const ServerVisualizerStreamObject &obj) { this->on_stream_start(obj); },
      [this]() { this->on_stream_end(); }, [this]() { this->on_stream_clear(); });
}

void SendspinVisualizer::loop() {
  if (!this->stream_active_ || this->ring_count_ == 0) {
    return;
  }

  const int64_t now = esp_timer_get_time();
  bool updated = false;

  // Advance through frames whose display time has passed, applying the most recent one.
  // Timestamps are stored as raw server times and converted here on the main thread,
  // which is safe (hub state only accessed from main thread) and uses the latest time sync.
  while (this->ring_count_ > 0) {
    VisualizerFrame *frame = this->ring_frame_(this->ring_read_);

    int64_t display_time = this->parent_->get_client_time(frame->server_time);
    if (display_time == 0) {
      // Time sync not available yet - discard frame
      this->ring_read_ = (this->ring_read_ + 1) % this->ring_capacity_;
      this->ring_count_--;
      continue;
    }

    if (display_time > now) {
      break;
    }

    // This frame's time has passed - apply it
    this->loudness_ = frame->loudness;
    this->peak_frequency_ = frame->peak_frequency;

    // Copy spectrum data from the ring slot to the display buffer
    if (this->spectrum_buffer_ != nullptr && this->spectrum_bin_count_ > 0) {
      uint16_t *src = this->ring_spectrum_(this->ring_read_);
      std::memcpy(this->spectrum_buffer_.get(), src, this->spectrum_bin_count_ * sizeof(uint16_t));
    }

    updated = true;

    // Advance read pointer
    this->ring_read_ = (this->ring_read_ + 1) % this->ring_capacity_;
    this->ring_count_--;
  }

  if (updated) {
    this->on_frame_callbacks_.call();
  }

  // Process beat events
  while (this->beat_count_ > 0) {
    int64_t server_time = this->beat_times_[this->beat_read_];
    int64_t beat_time = this->parent_->get_client_time(server_time);
    if (beat_time == 0) {
      this->beat_read_ = (this->beat_read_ + 1) % MAX_BUFFERED_BEATS;
      this->beat_count_--;
      continue;
    }
    if (beat_time > now) {
      break;
    }

    this->on_beat_callbacks_.call();
    this->beat_read_ = (this->beat_read_ + 1) % MAX_BUFFERED_BEATS;
    this->beat_count_--;
  }
}

void SendspinVisualizer::dump_config() {
  ESP_LOGCONFIG(TAG, "Sendspin Visualizer:");
  ESP_LOGCONFIG(TAG, "  Requested types:");
  for (const auto &type : this->requested_types_) {
    ESP_LOGCONFIG(TAG, "    - %s", to_cstr(type));
  }
  ESP_LOGCONFIG(TAG, "  Buffer capacity: %zu bytes", this->buffer_capacity_);
  ESP_LOGCONFIG(TAG, "  Batch max: %u", this->batch_max_);
  ESP_LOGCONFIG(TAG, "  Frame ring: %zu slots x %zu bytes = %zu bytes (from %zu byte buffer_capacity)",
                this->ring_capacity_, this->frame_slot_size_, this->ring_capacity_ * this->frame_slot_size_,
                this->buffer_capacity_);
  if (this->spectrum_config_.has_value()) {
    const auto &spec = this->spectrum_config_.value();
    ESP_LOGCONFIG(TAG, "  Spectrum config:");
    ESP_LOGCONFIG(TAG, "    Bins: %u", spec.n_disp_bins);
    ESP_LOGCONFIG(TAG, "    Scale: %s", to_cstr(spec.scale));
    ESP_LOGCONFIG(TAG, "    Frequency range: %u - %u Hz", spec.f_min, spec.f_max);
    ESP_LOGCONFIG(TAG, "    Max rate: %u Hz", spec.rate_max);
  }
}

void SendspinVisualizer::set_spectrum_config(uint8_t n_disp_bins, VisualizerSpectrumScale scale, uint16_t f_min,
                                             uint16_t f_max, uint16_t rate_max) {
  this->spectrum_config_ = VisualizerSpectrumConfig{
      .n_disp_bins = n_disp_bins,
      .scale = scale,
      .f_min = f_min,
      .f_max = f_max,
      .rate_max = rate_max,
  };
}

VisualizerFrame *SendspinVisualizer::ring_frame_(size_t index) {
  return reinterpret_cast<VisualizerFrame *>(this->frame_ring_.get() + index * this->frame_slot_size_);
}

uint16_t *SendspinVisualizer::ring_spectrum_(size_t index) {
  return reinterpret_cast<uint16_t *>(this->frame_ring_.get() + index * this->frame_slot_size_ +
                                      sizeof(VisualizerFrame));
}

void SendspinVisualizer::parse_frame_data_(const uint8_t *data, size_t length, VisualizerFrame *frame,
                                           uint16_t *spectrum_dest) {
  size_t offset = 0;

  frame->loudness = 0;
  frame->peak_frequency = 0;

  for (const auto &type : this->active_types_) {
    switch (type) {
      case VisualizerDataType::LOUDNESS:
        if (offset + 2 <= length) {
          frame->loudness = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
          offset += 2;
        }
        break;
      case VisualizerDataType::F_PEAK:
        if (offset + 2 <= length) {
          frame->peak_frequency = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
          offset += 2;
        }
        break;
      case VisualizerDataType::SPECTRUM: {
        size_t stream_spectrum_bytes = this->stream_bin_count_ * 2;
        if (spectrum_dest != nullptr && offset + stream_spectrum_bytes <= length) {
          uint8_t bins_to_read = std::min(this->stream_bin_count_, this->configured_bin_count_);
          for (uint8_t i = 0; i < bins_to_read; i++) {
            spectrum_dest[i] = (static_cast<uint16_t>(data[offset + i * 2]) << 8) | data[offset + i * 2 + 1];
          }
        }
        offset += stream_spectrum_bytes;
        break;
      }
      default:
        break;
    }
  }
}

void SendspinVisualizer::on_visualizer_data(const uint8_t *data, size_t length) {
  if (!this->stream_active_ || length < VISUALIZER_BINARY_HEADER_SIZE) {
    return;
  }

  uint8_t num_frames = data[1];
  if (num_frames == 0) {
    return;
  }

  const size_t frame_size = FRAME_TIMESTAMP_SIZE + this->frame_data_size_;
  size_t offset = VISUALIZER_BINARY_HEADER_SIZE;

  // Parse all frames and store in ring buffer with raw server timestamps.
  // Timestamp conversion happens in loop() on the main thread for thread safety.
  for (uint8_t f = 0; f < num_frames; f++) {
    if (offset + frame_size > length) {
      ESP_LOGW(TAG, "Visualizer message too short at frame %u/%u", f, num_frames);
      break;
    }

    // Extract server timestamp (big-endian int64)
    int64_t server_time = 0;
    for (int i = 0; i < 8; i++) {
      server_time = (server_time << 8) | data[offset + i];
    }
    offset += FRAME_TIMESTAMP_SIZE;

    // If ring buffer is full, drop this incoming frame (keep oldest - they display soonest)
    if (this->ring_count_ >= this->ring_capacity_) {
      offset += this->frame_data_size_;
      continue;
    }

    // Write frame into ring buffer
    VisualizerFrame *frame = this->ring_frame_(this->ring_write_);
    uint16_t *spectrum_dest = this->ring_spectrum_(this->ring_write_);
    frame->server_time = server_time;

    this->parse_frame_data_(data + offset, length - offset, frame, spectrum_dest);
    offset += this->frame_data_size_;

    this->ring_write_ = (this->ring_write_ + 1) % this->ring_capacity_;
    this->ring_count_++;
  }
}

void SendspinVisualizer::on_beat_data(const uint8_t *data, size_t length) {
  if (!this->stream_active_ || length < VISUALIZER_BINARY_HEADER_SIZE) {
    return;
  }

  uint8_t num_frames = data[1];
  if (num_frames == 0) {
    return;
  }

  size_t offset = VISUALIZER_BINARY_HEADER_SIZE;

  for (uint8_t f = 0; f < num_frames; f++) {
    if (offset + FRAME_TIMESTAMP_SIZE > length) {
      break;
    }

    // Extract server timestamp (raw, converted in loop())
    int64_t server_time = 0;
    for (int i = 0; i < 8; i++) {
      server_time = (server_time << 8) | data[offset + i];
    }
    offset += FRAME_TIMESTAMP_SIZE;

    // If beat buffer is full, drop incoming (keep oldest - they fire soonest)
    if (this->beat_count_ >= MAX_BUFFERED_BEATS) {
      continue;
    }

    this->beat_times_[this->beat_write_] = server_time;
    this->beat_write_ = (this->beat_write_ + 1) % MAX_BUFFERED_BEATS;
    this->beat_count_++;
  }
}

void SendspinVisualizer::on_stream_start(const ServerVisualizerStreamObject &stream_obj) {
  ESP_LOGD(TAG, "Visualizer stream started");

  this->active_types_ = stream_obj.types;
  this->stream_active_ = true;

  // Store the server's spectrum bin count (may differ from what we requested)
  this->stream_bin_count_ = 0;
  if (stream_obj.spectrum.has_value()) {
    this->stream_bin_count_ = stream_obj.spectrum.value().n_disp_bins;
  }

  // Compute frame data size from the types array (excluding beat, which uses separate messages)
  this->frame_data_size_ = 0;
  for (const auto &type : this->active_types_) {
    switch (type) {
      case VisualizerDataType::LOUDNESS:
        this->frame_data_size_ += 2;
        break;
      case VisualizerDataType::F_PEAK:
        this->frame_data_size_ += 2;
        break;
      case VisualizerDataType::SPECTRUM:
        this->frame_data_size_ += this->stream_bin_count_ * 2;
        break;
      default:
        break;
    }
  }

  // Update the base class bin count to reflect what we can actually display
  this->spectrum_bin_count_ = std::min(this->stream_bin_count_, this->configured_bin_count_);

  // Reset ring buffer
  this->ring_write_ = 0;
  this->ring_read_ = 0;
  this->ring_count_ = 0;
  this->beat_write_ = 0;
  this->beat_read_ = 0;
  this->beat_count_ = 0;

  ESP_LOGD(TAG, "  Types: %zu, frame data size: %zu bytes, spectrum bins: %u (server: %u, configured: %u)",
           this->active_types_.size(), this->frame_data_size_, this->spectrum_bin_count_, this->stream_bin_count_,
           this->configured_bin_count_);
}

void SendspinVisualizer::on_stream_end() {
  ESP_LOGD(TAG, "Visualizer stream ended");
  this->stream_active_ = false;
  this->active_types_.clear();
  this->frame_data_size_ = 0;

  // Reset ring buffer and values
  this->ring_write_ = 0;
  this->ring_read_ = 0;
  this->ring_count_ = 0;
  this->beat_write_ = 0;
  this->beat_read_ = 0;
  this->beat_count_ = 0;

  this->loudness_ = 0;
  this->peak_frequency_ = 0;
  if (this->spectrum_buffer_ != nullptr) {
    std::memset(this->spectrum_buffer_.get(), 0, this->configured_bin_count_ * sizeof(uint16_t));
  }
}

void SendspinVisualizer::on_stream_clear() {
  ESP_LOGD(TAG, "Visualizer stream clear");
  // Clear buffered data but keep stream active
  this->ring_write_ = 0;
  this->ring_read_ = 0;
  this->ring_count_ = 0;
  this->beat_write_ = 0;
  this->beat_read_ = 0;
  this->beat_count_ = 0;

  this->loudness_ = 0;
  this->peak_frequency_ = 0;
  if (this->spectrum_buffer_ != nullptr) {
    std::memset(this->spectrum_buffer_.get(), 0, this->configured_bin_count_ * sizeof(uint16_t));
  }
}

VisualizerSupportObject SendspinVisualizer::get_support_object() const {
  VisualizerSupportObject support;
  support.types = this->requested_types_;
  support.buffer_capacity = this->buffer_capacity_;
  support.batch_max = this->batch_max_;
  support.spectrum = this->spectrum_config_;
  return support;
}

}  // namespace sendspin
}  // namespace esphome

#endif  // USE_ESP32
