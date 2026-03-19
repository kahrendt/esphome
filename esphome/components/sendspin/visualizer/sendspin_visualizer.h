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
  /// Parses batched frames and updates the latest frame data.
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
  // Configuration
  std::vector<VisualizerDataType> requested_types_;
  size_t buffer_capacity_{8192};
  uint8_t batch_max_{4};
  std::optional<VisualizerSpectrumConfig> spectrum_config_;

  // Stream state (set from stream/start, cleared on stream/end)
  bool stream_active_{false};
  std::vector<VisualizerDataType> active_types_;  // Types in the current stream (order matters for parsing)
  size_t frame_data_size_{0};                     // Bytes per frame (excluding 8-byte timestamp)

  // Spectrum buffer (allocated once based on configured n_disp_bins)
  std::unique_ptr<uint16_t[]> spectrum_buffer_;
  uint8_t configured_bin_count_{0};

  // Pending state flags (set from hub callbacks, consumed in loop)
  bool pending_frame_{false};
  bool pending_beat_{false};
};

}  // namespace sendspin
}  // namespace esphome

#endif  // USE_ESP32
