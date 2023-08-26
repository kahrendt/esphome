/*
 * Based on https://github.com/tdunning/t-digest/blob/main/core/src/main/java/com/tdunning/math/stats/MergingDigest.java
 */

#include "merging_tdigest.h"

#include "esphome/core/helpers.h"  // necessary for ExternalRAMAllocator

#include <queue>
#include <algorithm>
#include <cmath>

namespace esphome {
namespace statistics_distribution {

static const double PI = 3.141592653589793;

Centroid::Centroid(double mean, size_t weight) {
  this->mean_ = mean;
  this->weight_ = weight;
}

void Centroid::update(double mean, size_t weight) {
  // Compute a weighted average of the old mean and weight with the new mean and weight
  this->mean_ = (this->mean_ * this->weight_ + mean * weight) / (this->weight_ + weight);
  this->weight_ += weight;
}

double K1Scale::q_max(double q, double normalizer) {
  if (q <= 0) {
    return 0;
  } else if (q >= 1) {
    return 0;
  }

  return 2 * std::sin(0.5 / normalizer) * std::sqrt(q * (1 - q));
}

double K1Scale::normalizer(size_t compression, size_t weight) { return compression / (PI); }

double K2Scale::q_max(double q, double normalizer) { return q * (1 - q) / normalizer; }
double K2Scale::normalizer(size_t compression, size_t weight) {
  double z = 4 * std::log(weight / compression) + 24;
  return compression / z;
}

double K3Scale::q_max(double q, double normalizer) { return std::min(q, 1 - q) / normalizer; }
double K3Scale::normalizer(size_t compression, size_t weight) {
  double z = 4 * std::log(weight / compression) + 21;
  return compression / z;
}

MergingDigest::MergingDigest(size_t compression, ScaleFunctions scale_function, uint16_t buffer_size) {
  if (scale_function == K1_SCALE)
    this->scale_function_ = new K1Scale();
  else if (scale_function == K2_SCALE)
    this->scale_function_ = new K2Scale();
  else if (scale_function == K3_SCALE)
    this->scale_function_ = new K3Scale();

  this->compression_ = compression;
  this->buffer_size_ = buffer_size;

  this->clear();
}

void MergingDigest::add(double x, size_t w) {
  if (std::isnan(x))
    return;

  if (this->temporary_buffer_.size() >= this->buffer_size_) {
    this->merge_new_values_();
  }

  this->temporary_buffer_.push_back(Centroid(x, w));

  this->unmerged_weight_ += w;

  if (x < this->min_) {
    this->min_ = x;
  }
  if (x > this->max_) {
    this->max_ = x;
  }
}

void MergingDigest::clear() {
  this->centroids_vector_.clear();
  this->temporary_buffer_.clear();
}

void MergingDigest::compress_for_saving(uint8_t max_centroids, Centroid tdigest_array[100]) {
  this->merge_new_values_();

  std::vector<Centroid, ExternalRAMAllocator<Centroid>>
      tdigest_vector;  // vector to store the tdigest with compression=max_centroids/2

  this->merge_(max_centroids / 2, &tdigest_vector);

  for (uint8_t i = 0; i < max_centroids; ++i) {
    if ((i < tdigest_vector.size()) && (tdigest_vector.size() > 0) && (tdigest_vector[i].get_weight() > 0)) {
      tdigest_array[i] = tdigest_vector[i];
    } else {
      tdigest_array[i] = Centroid(NAN, 0);  // Pad unused elements with a null Centroid
    }
  }
}

void MergingDigest::merge_new_values_() {
  if (this->unmerged_weight_ > 0) {
    this->merge_(this->compression_, &this->centroids_vector_);
  }
}

void MergingDigest::merge_(uint16_t compression,
                           std::vector<Centroid, ExternalRAMAllocator<Centroid>> *tdigest_vector) {
  if ((this->total_weight_ == 0) && (this->unmerged_weight_ == 0))
    return;

  //  Append merged centroids with temp_centroids
  this->temporary_buffer_.insert(this->temporary_buffer_.end(), this->centroids_vector_.begin(),
                                 this->centroids_vector_.end());

  std::stable_sort(&this->temporary_buffer_.front(), &this->temporary_buffer_.back(),
                   [](const Centroid &c1, const Centroid &c2) { return (c1.get_mean() < c2.get_mean()); });

  this->total_weight_ += this->unmerged_weight_;

  tdigest_vector->clear();
  tdigest_vector->push_back(this->temporary_buffer_.front());

  size_t weight_so_far = 0;

  double normalizer = this->scale_function_->normalizer(compression, this->total_weight_);

  for (auto it = this->temporary_buffer_.begin() + 1; it != this->temporary_buffer_.end(); ++it) {
    size_t proposed_weight = it->get_weight() + this->centroids_vector_.back().get_weight();
    size_t projected_weight = weight_so_far + proposed_weight;

    bool add_this;

    double q0 = static_cast<double>(weight_so_far) / static_cast<double>(this->total_weight_);
    double q2 = static_cast<double>(projected_weight) / static_cast<double>(this->total_weight_);

    double q0_max = this->scale_function_->q_max(q0, normalizer);
    double q2_max = this->scale_function_->q_max(q2, normalizer);

    double factor = std::min(q0_max, q2_max);

    add_this = (proposed_weight <= this->total_weight_ * factor);

    if ((it == this->temporary_buffer_.begin() + 1) || (it == this->temporary_buffer_.end() - 1))
      add_this = false;

    if (add_this) {
      // next point will fit, so merge into existing centroid
      tdigest_vector->back().update(it->get_mean(), it->get_weight());

      it->set_weight(0);
    } else {
      // didn't fit, move to next output, copy out first centroid
      weight_so_far += tdigest_vector->back().get_weight();

      tdigest_vector->push_back(*it);

      it->set_weight(0);
    }
  }

  if (this->total_weight_ > 0) {
    this->min_ = std::min(this->min_, static_cast<float>(tdigest_vector->front().get_mean()));
    this->max_ = std::max(this->max_, static_cast<float>(tdigest_vector->back().get_mean()));
  }

  this->temporary_buffer_.clear();
  this->unmerged_weight_ = 0;
}

double MergingDigest::cdf(double x) {
  if (this->unmerged_weight_ > 0)
    this->merge_new_values_();

  if (this->centroids_vector_.size() == 0) {
    return NAN;
  }

  if (x < this->min_) {
    return 0.0;
  } else if (x > this->max_) {
    return 1.0;
  } else if (this->centroids_vector_.size() == 1) {
    return 0.5;
  }

  if (x < this->centroids_vector_.front().get_mean()) {
    if (this->centroids_vector_.front().get_mean() - this->min_) {
      if (x == this->min_) {
        return 0.5 / this->total_weight_;
      } else {
        return (1.0 + (x - this->min_) / (this->centroids_vector_.front().get_mean() - this->min_) *
                          (this->centroids_vector_.front().get_weight() / 2.0 - 1.0)) /
               this->total_weight_;
      }
    } else {
      return 0.0;
    }
  }

  if (x > this->centroids_vector_.back().get_mean()) {
    if (this->max_ - this->centroids_vector_.back().get_mean() > 0) {
      if (x == this->max_) {
        return 1.0 - 0.5 / this->total_weight_;
      } else {
        double dq = (1.0 + (this->max_ - x) / (this->max_ - this->centroids_vector_.back().get_mean()) *
                               (this->centroids_vector_.back().get_weight() / 2.0 - 1.0)) /
                    this->total_weight_;
        return 1.0 - dq;
      }
    } else {
      return 1;
    }
  }

  double weight_so_far = 0.0;

  for (auto it = this->temporary_buffer_.begin(); it != this->temporary_buffer_.end(); ++it) {
    // NOTE not checking for "floating point madness"...

    if ((it->get_mean() <= x) && (x < (it + 1)->get_mean())) {
      // Centroids it and it+1 bracket the value

      double left_exclude_weight = 0.0;
      double right_exclude_weight = 0.0;
      if (it->get_weight() == 1) {
        if ((it + 1)->get_weight() == 1) {
          // Two singletons means do not interpolate
          return (weight_so_far + 1.0) / this->total_weight_;
        } else {
          left_exclude_weight = 0.5;
        }
      } else if ((it + 1)->get_weight() == 1) {
        right_exclude_weight = 0.5;
      }

      double dw = (it->get_weight() + (it + 1)->get_weight()) / 2.0;

      double left = it->get_mean();
      double right = (it + 1)->get_mean();

      double dw_no_singleton = dw - left_exclude_weight - right_exclude_weight;

      double base = weight_so_far + it->get_weight() / 2.0 + left_exclude_weight;

      return (base + dw_no_singleton * (x - left) / (right - left)) / this->total_weight_;
    } else {
      weight_so_far += it->get_weight();
    }
  }

  return 1.0 - 0.5 / this->total_weight_;
}

double MergingDigest::quantile(double q) {
  if (this->unmerged_weight_ > 0)
    this->merge_new_values_();

  if (this->centroids_vector_.size() == 0) {
    return NAN;
  }

  if (this->centroids_vector_.size() == 1)
    return this->centroids_vector_.front().get_mean();

  double index = q * this->total_weight_;

  if (index < 1)
    return this->min_;

  if ((this->centroids_vector_.front().get_weight() > 1) &&
      (index < this->centroids_vector_.front().get_weight() / 2.0))
    return this->min_ + (index - 1.0) / (this->centroids_vector_.front().get_weight() / 2.0 - 1.0) *
                            (this->centroids_vector_.front().get_mean() - this->min_);

  if (index > this->total_weight_ - 1)
    return this->max_;

  if ((this->centroids_vector_.back().get_weight() > 1) &&
      (this->total_weight_ - index <= this->centroids_vector_.back().get_weight() / 2.0))
    return this->max_ - (this->total_weight_ - index - 1.0) /
                            (this->centroids_vector_.back().get_weight() / 2.0 - 1.0) *
                            (this->max_ - this->centroids_vector_.back().get_weight());

  double weight_so_far = this->centroids_vector_.front().get_weight() / 2.0;

  for (auto it = this->centroids_vector_.begin(); it != this->centroids_vector_.end(); ++it) {
    double dw = (it->get_weight() + (it + 1)->get_weight()) / 2.0;

    if (weight_so_far + dw > index) {
      // Centroids at i and i+1 bracket the point

      double left_unit = 0.0;
      if (it->get_weight() == 1) {
        if (index - weight_so_far < 0.5) {
          return it->get_mean();
        } else {
          left_unit = 0.5;
        }
      }

      double right_unit = 0.0;
      if ((it + 1)->get_weight() == 1) {
        if (weight_so_far + dw - index <= 0.5) {
          return (it + 1)->get_mean();
        } else {
          right_unit = 0.5;
        }
      }

      double z1 = index - weight_so_far - left_unit;
      double z2 = weight_so_far + dw - index - right_unit;

      return (it->get_mean() * z2 + (it + 1)->get_mean() * z1) / (z1 + z2);
    }
    weight_so_far += dw;
  }

  double z1 = index - this->total_weight_ - this->centroids_vector_.back().get_weight() / 2.0;
  double z2 = this->centroids_vector_.back().get_weight() / 2.0 - z1;
  return (this->centroids_vector_.back().get_mean() * z1 + this->max_ * z2) / (z1 + z2);
}

}  // namespace statistics_distribution
}  // namespace esphome
