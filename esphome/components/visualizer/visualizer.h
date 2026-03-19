#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <cstring>
#include <functional>

namespace esphome::visualizer {

class Visualizer {
 public:
  virtual ~Visualizer() = default;

  // --- Polling API (latest frame data) ---

  /// @brief Get the current loudness value (0-65535, perceptually weighted).
  uint16_t get_loudness() const { return this->loudness_; }

  /// @brief Get the peak frequency in Hz (0 = no peak detected).
  uint16_t get_peak_frequency() const { return this->peak_frequency_; }

  /// @brief Get a pointer to the spectrum bin array, or nullptr if no spectrum data.
  const uint16_t *get_spectrum_bins() const { return this->spectrum_bins_; }

  /// @brief Get the number of spectrum bins.
  uint8_t get_spectrum_bin_count() const { return this->spectrum_bin_count_; }

  // --- Callback API ---

  /// @brief Register a callback invoked when a new visualization frame is available.
  void add_on_frame_callback(std::function<void()> &&callback) { this->on_frame_callbacks_.add(std::move(callback)); }

  /// @brief Register a callback invoked when a beat event occurs.
  void add_on_beat_callback(std::function<void()> &&callback) { this->on_beat_callbacks_.add(std::move(callback)); }

 protected:
  /// @brief Update frame data and fire on_frame callbacks.
  void publish_frame_(uint16_t loudness, uint16_t peak_freq, const uint16_t *spectrum, uint8_t spectrum_count) {
    this->loudness_ = loudness;
    this->peak_frequency_ = peak_freq;
    if (spectrum != nullptr && spectrum_count > 0 && this->spectrum_bins_ != nullptr) {
      std::memcpy(this->spectrum_bins_, spectrum, spectrum_count * sizeof(uint16_t));
      this->spectrum_bin_count_ = spectrum_count;
    }
    this->on_frame_callbacks_.call();
  }

  /// @brief Fire on_beat callbacks.
  void publish_beat_() { this->on_beat_callbacks_.call(); }

  uint16_t loudness_{0};
  uint16_t peak_frequency_{0};
  uint16_t *spectrum_bins_{nullptr};  // Owned by implementation subclass
  uint8_t spectrum_bin_count_{0};

  CallbackManager<void()> on_frame_callbacks_;
  CallbackManager<void()> on_beat_callbacks_;
};

}  // namespace esphome::visualizer
