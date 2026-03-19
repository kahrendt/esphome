#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <cstring>
#include <functional>

namespace esphome::visualizer {

/// @brief Display-ready state for a single spectrum bin, with smoothing and pixel-level dirty tracking.
struct DisplayBin {
  float smoothed;       // Current smoothed value (attack/decay applied)
  float peak;           // Peak hold value (decays slower)
  uint16_t last_pixel;  // Last pixel position sent to display (for dirty detection)
  uint16_t last_peak_pixel;
};

class Visualizer {
 public:
  virtual ~Visualizer() = default;

  // --- Raw data API (latest frame, no smoothing) ---

  /// @brief Get the current loudness value (0-65535, perceptually weighted).
  uint16_t get_loudness() const { return this->loudness_; }

  /// @brief Get the peak frequency in Hz (0 = no peak detected).
  uint16_t get_peak_frequency() const { return this->peak_frequency_; }

  /// @brief Get a pointer to the spectrum bin array, or nullptr if no spectrum data.
  const uint16_t *get_spectrum_bins() const { return this->spectrum_bins_; }

  /// @brief Get the number of spectrum bins.
  uint8_t get_spectrum_bin_count() const { return this->spectrum_bin_count_; }

  // --- Display API (smoothed, quantized, with dirty tracking) ---

  /// @brief Configure display parameters. Must be called before using display API.
  /// @param display_height Height in pixels of the display area (used for quantization).
  /// @param bar_decay Decay multiplier per update_display() call (e.g., 0.88 = 12% drop per tick).
  /// @param peak_decay Decay multiplier for peak hold per tick (e.g., 0.97).
  /// @param peak_fall_speed Constant subtraction from peak per tick (in raw units, e.g., 400).
  void configure_display(uint16_t display_height, float bar_decay, float peak_decay, float peak_fall_speed);

  /// @brief Update smoothed display values from raw data. Call this at your display refresh rate.
  /// Applies attack/decay smoothing, updates peak hold, and quantizes to pixel resolution.
  /// After calling, use get_dirty_count() and iterate with get_dirty_bin_index() to find changed bins.
  void update_display();

  /// @brief Get the smoothed bar pixel height for a bin (0 = empty, display_height = full).
  uint16_t get_bar_pixel(uint8_t index) const;

  /// @brief Get the peak hold pixel position for a bin.
  uint16_t get_peak_pixel(uint8_t index) const;

  /// @brief Get the smoothed loudness pixel value (0 to display_height).
  uint16_t get_loudness_pixel() const { return this->loudness_pixel_; }

  /// @brief Returns true if the loudness pixel changed since last update_display().
  bool is_loudness_dirty() const { return this->loudness_dirty_; }

  /// @brief Get the number of bins whose pixel position changed since last update_display().
  uint8_t get_dirty_count() const { return this->dirty_count_; }

  /// @brief Get the bin index of the Nth dirty bin (0-based). Only valid for n < get_dirty_count().
  uint8_t get_dirty_bin_index(uint8_t n) const { return this->dirty_indices_[n]; }

  /// @brief Returns true if the peak dot for the given dirty bin moved or changed visibility.
  /// Only meaningful for bins returned by get_dirty_bin_index().
  bool is_peak_dirty(uint8_t index) const;

  /// @brief Returns true if the peak dot should be visible (floating above bar).
  bool is_peak_visible(uint8_t index) const;

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

  // Raw frame data
  uint16_t loudness_{0};
  uint16_t peak_frequency_{0};
  uint16_t *spectrum_bins_{nullptr};  // Owned by implementation subclass
  uint8_t spectrum_bin_count_{0};

  CallbackManager<void()> on_frame_callbacks_;
  CallbackManager<void()> on_beat_callbacks_;

  // Display state
  uint16_t display_height_{0};
  float bar_decay_{0.88f};
  float peak_decay_{0.97f};
  float peak_fall_speed_{400.0f};
  bool display_configured_{false};

  DisplayBin *display_bins_{nullptr};  // Owned by implementation (allocated alongside spectrum buffer)
  float smoothed_loudness_{0.0f};
  uint16_t loudness_pixel_{0};
  bool loudness_dirty_{false};

  // Dirty tracking: indices of bins whose pixel changed
  uint8_t dirty_indices_[128]{};  // Max 128 bins
  uint8_t dirty_count_{0};
};

}  // namespace esphome::visualizer
