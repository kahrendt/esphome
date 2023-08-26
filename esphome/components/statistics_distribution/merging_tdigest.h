/*
 * Based on https://github.com/tdunning/t-digest/blob/main/core/src/main/java/com/tdunning/math/stats/MergingDigest.java
 */

#pragma once

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

#include <algorithm>
#include <cmath>

namespace esphome {
namespace statistics_distribution {

enum ScaleFunctions {
  K1_SCALE,
  K2_SCALE,
  K3_SCALE,
};

// Centroids are bins that store the average value of all the weight measurements in the bin
class Centroid {
 public:
  Centroid(){};

  Centroid(double mean, size_t weight);

  void update(double mean, size_t weight);

  double get_mean() const { return this->mean_; };

  size_t get_weight() const { return this->weight_; }
  void set_weight(size_t weight) { this->weight_ = weight; }

 private:
  double mean_ = 0.0;
  size_t weight_ = 0;
};

class ScaleFunction {
 public:
  virtual double q_max(double q, double normalizer) = 0;
  virtual double normalizer(size_t compression, size_t weight) = 0;
};

class K1Scale : public ScaleFunction {
 public:
  double q_max(double q, double normalizer);
  double normalizer(size_t compression, size_t weight);
};

class K2Scale : public ScaleFunction {
 public:
  double q_max(double q, double normalizer);

  double normalizer(size_t compression, size_t weight);
};

class K3Scale : public ScaleFunction {
 public:
  double q_max(double q, double normalizer);
  double normalizer(size_t compression, size_t weight);
};

class MergingDigest {
 public:
  MergingDigest(size_t compression, ScaleFunctions scale_function, uint16_t buffer_size);

  void add(double x, size_t w);

  double quantile(double q);

  double cdf(double x);

  void clear();

  uint16_t get_centroids_count() { return this->centroids_vector_.size(); }
  // uint16_t get_unmerged_count() { return this->temporary_buffer_.size(); }
  size_t get_total_weight() { return this->total_weight_; }

  float get_max() { return this->max_; }
  float get_min() { return this->min_; }

  void compress_for_saving(uint8_t max_centroids, Centroid *tdigest_array);

 protected:
  ScaleFunction *scale_function_{nullptr};

  float min_{};
  float max_{};

  size_t total_weight_{0};
  size_t unmerged_weight_{0};

  uint16_t compression_{};
  uint16_t buffer_size_{};

  void merge_(uint16_t compression, std::vector<Centroid, ExternalRAMAllocator<Centroid>> *tdigest_vector);
  void merge_new_values_();

  std::vector<Centroid, ExternalRAMAllocator<Centroid>> centroids_vector_{};
  std::vector<Centroid, ExternalRAMAllocator<Centroid>> temporary_buffer_{};
};

}  // namespace statistics_distribution
}  // namespace esphome
