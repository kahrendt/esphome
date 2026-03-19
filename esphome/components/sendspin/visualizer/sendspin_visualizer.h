#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/visualizer/visualizer.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../sendspin_protocol.h"

#include <memory>
#include <vector>

namespace esphome {
namespace sendspin {

class SendspinHub;

/// @brief A single parsed visualizer frame ready for display.
struct VisualizerFrame {
  int64_t server_time;  // Raw server timestamp in microseconds (converted to local time in loop())
  uint16_t loudness;
  uint16_t peak_frequency;
  // Spectrum bins are stored inline after this struct in the ring buffer
};

class SendspinVisualizer : public Component, public visualizer::Visualizer, public Parented<SendspinHub> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // --- Config setters (called from code generation) ---

  void add_data_type(VisualizerDataType type) { this->requested_types_.push_back(type); }
  void set_buffer_capacity(size_t capacity) { this->buffer_capacity_ = capacity; }
  void set_batch_max(uint8_t batch_max) { this->batch_max_ = batch_max; }
  void set_spectrum_config(uint8_t n_disp_bins, VisualizerSpectrumScale scale, uint16_t f_min, uint16_t f_max,
                           uint16_t rate_max);

  // --- Hub callbacks ---

  /// @brief Called by hub when a visualizer binary message (type 16) arrives.
  /// Parses all frames and stores them in the ring buffer with converted timestamps.
  /// @param data Pointer to the full binary message (starting at byte 0 = message type).
  /// @param length Total message length in bytes.
  void on_visualizer_data(const uint8_t *data, size_t length);

  /// @brief Called by hub when a beat binary message (type 17) arrives.
  /// @param data Pointer to the full binary message (starting at byte 0 = message type).
  /// @param length Total message length in bytes.
  void on_beat_data(const uint8_t *data, size_t length);

  /// @brief Called by hub when stream/start includes a visualizer object.
  void on_stream_start(const ServerVisualizerStreamObject &stream_obj);

  /// @brief Called by hub when stream/end includes the visualizer role.
  void on_stream_end();

  /// @brief Called by hub when stream/clear includes the visualizer role.
  void on_stream_clear();

  // --- Support object for client/hello ---

  /// @brief Builds the VisualizerSupportObject for the client/hello message.
  VisualizerSupportObject get_support_object() const;

 protected:
  /// @brief Parse a single frame's data fields from the binary message and write into the frame ring buffer.
  /// @param data Pointer to the start of the data fields (after the 8-byte timestamp).
  /// @param length Remaining bytes in the message from this offset.
  /// @param frame Pointer to the VisualizerFrame to write scalar fields into.
  /// @param spectrum_dest Pointer to the spectrum buffer for this frame slot.
  void parse_frame_data_(const uint8_t *data, size_t length, VisualizerFrame *frame, uint16_t *spectrum_dest);

  // Configuration
  std::vector<VisualizerDataType> requested_types_;
  size_t buffer_capacity_{8192};
  uint8_t batch_max_{4};
  std::optional<VisualizerSpectrumConfig> spectrum_config_;

  // Stream state (set from stream/start, cleared on stream/end)
  bool stream_active_{false};
  std::vector<VisualizerDataType> active_types_;  // Types in the current stream (order matters for parsing)
  size_t frame_data_size_{0};                     // Bytes per frame (excluding 8-byte timestamp)
  uint8_t stream_bin_count_{0};                   // Server's bin count for this stream (for frame parsing)

  // Spectrum display buffer (allocated once based on configured n_disp_bins, used by base class)
  std::unique_ptr<uint16_t[]> spectrum_buffer_;
  std::unique_ptr<visualizer::DisplayBin[]> display_bins_storage_;
  uint8_t configured_bin_count_{0};

  // --- Frame ring buffer ---
  // Stores parsed frames with display timestamps. Each slot = VisualizerFrame + spectrum bins.
  // Size is derived from buffer_capacity / wire_frame_size in setup().
  std::unique_ptr<uint8_t[]> frame_ring_;  // Raw storage: ring_capacity_ * frame_slot_size_
  size_t frame_slot_size_{0};              // sizeof(VisualizerFrame) + configured_bin_count_ * sizeof(uint16_t)
  size_t ring_capacity_{0};                // Total slots (computed in setup)
  size_t ring_write_{0};                   // Next write index (0..ring_capacity_-1)
  size_t ring_read_{0};                    // Next read index
  size_t ring_count_{0};                   // Number of frames in buffer

  /// @brief Get a pointer to the frame at the given ring buffer index.
  VisualizerFrame *ring_frame_(size_t index);
  /// @brief Get the spectrum data pointer for the frame at the given ring buffer index.
  uint16_t *ring_spectrum_(size_t index);

  // --- Beat timing ---
  static const size_t MAX_BUFFERED_BEATS = 32;
  int64_t beat_times_[MAX_BUFFERED_BEATS]{};
  size_t beat_write_{0};
  size_t beat_read_{0};
  size_t beat_count_{0};
};

}  // namespace sendspin
}  // namespace esphome

#endif  // USE_ESP32
