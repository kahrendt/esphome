#include "visualizer.h"

namespace esphome::visualizer {

void Visualizer::configure_display(uint16_t display_height, float bar_decay, float peak_decay, float peak_fall_speed) {
  this->display_height_ = display_height;
  this->bar_decay_ = bar_decay;
  this->peak_decay_ = peak_decay;
  this->peak_fall_speed_ = peak_fall_speed;
  this->display_configured_ = true;
}

void Visualizer::update_display() {
  if (!this->display_configured_ || this->display_bins_ == nullptr) {
    return;
  }

  const float scale = (float) this->display_height_ / 65535.0f;
  this->dirty_count_ = 0;

  for (uint8_t i = 0; i < this->spectrum_bin_count_; i++) {
    DisplayBin &bin = this->display_bins_[i];
    float target = (this->spectrum_bins_ != nullptr) ? (float) this->spectrum_bins_[i] : 0.0f;

    // Bar: fast attack, slow decay
    if (target >= bin.smoothed) {
      bin.smoothed = target;
    } else {
      bin.smoothed *= this->bar_decay_;
      if (bin.smoothed < 100.0f)
        bin.smoothed = 0.0f;
    }

    // Peak hold: fast attack, slower decay + constant fall
    if (target >= bin.peak) {
      bin.peak = target;
    } else {
      bin.peak = bin.peak * this->peak_decay_ - this->peak_fall_speed_;
      if (bin.peak < 0.0f)
        bin.peak = 0.0f;
    }

    // Quantize to pixel positions
    uint16_t bar_pixel = (uint16_t) (bin.smoothed * scale);
    uint16_t peak_pixel = (uint16_t) (bin.peak * scale);

    // Check if anything changed at pixel level
    if (bar_pixel != bin.last_pixel || peak_pixel != bin.last_peak_pixel) {
      this->dirty_indices_[this->dirty_count_++] = i;
      bin.last_pixel = bar_pixel;
      bin.last_peak_pixel = peak_pixel;
    }
  }

  // Loudness smoothing
  float loudness_target = (float) this->loudness_;
  if (loudness_target >= this->smoothed_loudness_) {
    this->smoothed_loudness_ = loudness_target;
  } else {
    this->smoothed_loudness_ *= this->bar_decay_ + 0.02f;  // Slightly slower decay for loudness
    if (this->smoothed_loudness_ < 100.0f)
      this->smoothed_loudness_ = 0.0f;
  }
  uint16_t new_loudness_pixel = (uint16_t) (this->smoothed_loudness_ * scale);
  this->loudness_dirty_ = (new_loudness_pixel != this->loudness_pixel_);
  this->loudness_pixel_ = new_loudness_pixel;
}

uint16_t Visualizer::get_bar_pixel(uint8_t index) const {
  if (this->display_bins_ == nullptr || index >= this->spectrum_bin_count_) {
    return 0;
  }
  return this->display_bins_[index].last_pixel;
}

uint16_t Visualizer::get_peak_pixel(uint8_t index) const {
  if (this->display_bins_ == nullptr || index >= this->spectrum_bin_count_) {
    return 0;
  }
  return this->display_bins_[index].last_peak_pixel;
}

bool Visualizer::is_peak_dirty(uint8_t index) const {
  // A peak is considered dirty if its pixel changed (checked in update_display via dirty_indices)
  // This is a convenience - if the bin is dirty, caller should check peak too
  return true;  // If the bin is in dirty list, peak may have changed
}

bool Visualizer::is_peak_visible(uint8_t index) const {
  if (this->display_bins_ == nullptr || index >= this->spectrum_bin_count_) {
    return false;
  }
  const DisplayBin &bin = this->display_bins_[index];
  // Visible when peak is meaningfully above the bar (at least 1 pixel gap)
  return bin.last_peak_pixel > bin.last_pixel + 1;
}

}  // namespace esphome::visualizer
