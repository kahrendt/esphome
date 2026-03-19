#include "sendspin_visualizer.h"

#ifdef USE_ESP32

#include "../sendspin_hub.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

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
  if (this->pending_frame_) {
    this->pending_frame_ = false;
    this->on_frame_callbacks_.call();
  }
  if (this->pending_beat_) {
    this->pending_beat_ = false;
    this->on_beat_callbacks_.call();
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

void SendspinVisualizer::on_visualizer_data(const uint8_t *data, size_t length) {
  if (!this->stream_active_ || length < VISUALIZER_BINARY_HEADER_SIZE) {
    return;
  }

  uint8_t num_frames = data[1];
  if (num_frames == 0) {
    return;
  }

  size_t offset = VISUALIZER_BINARY_HEADER_SIZE;
  const size_t frame_size = FRAME_TIMESTAMP_SIZE + this->frame_data_size_;

  // Process only the last frame (most recent data) for display
  // Skip to the last frame
  if (num_frames > 1) {
    size_t skip = (num_frames - 1) * frame_size;
    if (offset + skip + frame_size > length) {
      ESP_LOGW(TAG, "Visualizer message too short for %u frames", num_frames);
      return;
    }
    offset += skip;
  }

  if (offset + frame_size > length) {
    ESP_LOGW(TAG, "Visualizer message too short");
    return;
  }

  // Skip timestamp (8 bytes) - we display immediately for the latest frame
  offset += FRAME_TIMESTAMP_SIZE;

  // Parse data fields in the order specified by active_types_
  uint16_t loudness = 0;
  uint16_t peak_freq = 0;

  for (const auto &type : this->active_types_) {
    switch (type) {
      case VisualizerDataType::LOUDNESS:
        if (offset + 2 <= length) {
          loudness = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
          offset += 2;
        }
        break;
      case VisualizerDataType::F_PEAK:
        if (offset + 2 <= length) {
          peak_freq = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
          offset += 2;
        }
        break;
      case VisualizerDataType::SPECTRUM: {
        // stream_bin_count_ is the server's bin count (used for frame size/offset advancement)
        // configured_bin_count_ is our buffer size (used for how many bins we actually read)
        size_t stream_spectrum_bytes = this->stream_bin_count_ * 2;
        if (this->spectrum_buffer_ != nullptr && offset + stream_spectrum_bytes <= length) {
          uint8_t bins_to_read = std::min(this->stream_bin_count_, this->configured_bin_count_);
          for (uint8_t i = 0; i < bins_to_read; i++) {
            this->spectrum_buffer_[i] = (static_cast<uint16_t>(data[offset + i * 2]) << 8) | data[offset + i * 2 + 1];
          }
        }
        offset += stream_spectrum_bytes;
        break;
      }
      default:
        break;
    }
  }

  this->loudness_ = loudness;
  this->peak_frequency_ = peak_freq;
  this->pending_frame_ = true;
}

void SendspinVisualizer::on_beat_data(const uint8_t *data, size_t length) {
  if (!this->stream_active_ || length < VISUALIZER_BINARY_HEADER_SIZE) {
    return;
  }

  uint8_t num_frames = data[1];
  if (num_frames == 0) {
    return;
  }

  // Beat messages contain only timestamps, no other data
  // We just signal that a beat occurred
  this->pending_beat_ = true;
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

  ESP_LOGD(TAG, "  Types: %zu, frame data size: %zu bytes, spectrum bins: %u (server: %u, configured: %u)",
           this->active_types_.size(), this->frame_data_size_, this->spectrum_bin_count_, this->stream_bin_count_,
           this->configured_bin_count_);
}

void SendspinVisualizer::on_stream_end() {
  ESP_LOGD(TAG, "Visualizer stream ended");
  this->stream_active_ = false;
  this->active_types_.clear();
  this->frame_data_size_ = 0;

  // Reset values to zero
  this->loudness_ = 0;
  this->peak_frequency_ = 0;
  if (this->spectrum_buffer_ != nullptr) {
    std::memset(this->spectrum_buffer_.get(), 0, this->configured_bin_count_ * sizeof(uint16_t));
  }
}

void SendspinVisualizer::on_stream_clear() {
  ESP_LOGD(TAG, "Visualizer stream clear");
  // Clear buffered data but keep stream active
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
