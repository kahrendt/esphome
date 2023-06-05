/*
  Summary Partial statistics are stored in a circular queue with capacity window_size_
  One example of implementation: https://towardsdatascience.com/circular-queue-or-ring-buffer-92c7b0193326

  Summary statistics are computed using the DABA Lite algorithm
    - space requirements: n+2
    - time complexity: worse-case O(1)
    - based on: https://github.com/IBM/sliding-window-aggregators/blob/master/cpp/src/DABALite.hpp (Apache License)
*/

#pragma once

#include "esphome/core/helpers.h"
#include<limits>
#include<algorithm>
#include<vector>

namespace esphome {

namespace statistics {

// partial summary statistics structure
struct Partial {
  float max;
  float min;

  double m2;      // used to find variance/standard deviation via Welford's algorithm

  float mean;

  size_t count;
};


class DABALite {
 public:
  DABALite(size_t window_size) {
    this->window_size_ = window_size;
    this->q_.reserve(this->window_size_);
  }

  size_t size() {
    return this->queue_size_;
  }

  // insert a new value at end of circular queue and step DABA Lite algorithm
  void insert(float value) {
    Partial lifted = this->lift_(value);

    this->backSum_ = this->combine_(this->backSum_, lifted);

    if (this->size() > 0)
      this->tail_=return_next_(this->tail_);

    this->q_[this->tail_] = lifted;

    ++this->queue_size_;
    
    this->step_();
  }

  // remove value at start of circular queue and step DABA Lite algorithm
  void evict() {
    if (this->size() > 0) {
      this->head_ = this->return_next_(head_);
      --this->queue_size_;

      this->step_();          
    }
  }

  // Return the summary statistics for all entries in the queue
  Partial query() {
    if (this->size() > 0) {
      Partial alpha = this->get_alpha_();
      Partial back = this->get_back_();

      return this->combine_(alpha, back);
    }
    else
      return this->identity_;
  }

 protected:
  // vector to store summary statistics, will use it as a circular queue
  // std::vector<Partial> q_{};
  std::vector<Partial, ExternalRAMAllocator<Partial>> q_{};

  // size of the circular queue; i.e., number of valid entries
  size_t queue_size_{0};
  size_t window_size_{};

  // Circular Queue - Indices
  size_t head_{0};
  size_t tail_{0};

  // DABA Lite - Indices
  size_t l_{0};
  size_t r_{0};
  size_t a_{0};
  size_t b_{0};

  // Summary statisitics for a null entry
  Partial identity_{std::numeric_limits<float>::infinity()*(-1),std::numeric_limits<float>::infinity(),0.0,0.0,0};

  // DABA Lite - Running Totals
  Partial midSum_{this->identity_}, backSum_{this->identity_};

  // Circular Queue - next and previous indices in the circular queue
  inline size_t return_next_(size_t current) { return (current+1) % this->window_size_; }
  inline size_t return_previous_(size_t current) { return (current-1) % this->window_size_; }

  // compute summary statistics for a single new value
  Partial lift_(float v) {
    Partial part = this->identity_;

    if (!std::isnan(v)) {
      part.max = v;
      part.min = v;

      part.count = 1;
      
      part.m2 = 0.0;

      part.mean = v;
    }

    return part;
  }

  // combine summary statistics from two partial samples
  Partial combine_(Partial &a, Partial &b) {  
    Partial part;

    part.max = std::max(a.max, b.max);
    part.min = std::min(a.min, b.min);

    part.count = a.count + b.count;

    double a_count = static_cast<float>(a.count);
    double b_count = static_cast<float>(b.count); 
    double part_count = static_cast<double>(part.count);  

    // compute overall M2 using Chan's parallel algorithm for computing the variance
    float delta = b.mean - a.mean;    
    part.m2 = a.m2 + b.m2 + delta*delta*a_count*b_count/part_count;

    // weighted average of the two means based on their counts
    part.mean = (a.mean*a_count+b.mean*b_count)/part_count;

    return part;
  }


  // DABA Lite algorithm method
  void step_() {
    if (this->l_ == this->b_) {
      this->flip_();
    }

    if (this->head_ != this->b_) {
      if (a_ != r_) {
        Partial prev_delta = this->get_delta_();

        this->a_ = return_previous_(this->a_);
        this->q_[this->a_] = combine_(this->q_[this->a_], prev_delta);
      }

      if (this->l_ != this->r_) {
        this->q_[this->l_] = combine_(this->q_[this->l_], this->midSum_);

        this->l_ = return_next_(this->l_);
      }
      else {
        this->l_ = return_next_(this->l_);
        this->r_ = return_next_(this->r_);
        this->a_ = return_next_(this->a_);
        this->midSum_ = get_delta_();
      }
    }
    else {
      this->backSum_ = this->midSum_ = this->identity_;
    }
  }

  // DABA Lite algorithm method
  void flip_() {
    this->l_ = this->head_;
    this->r_ = this->b_;
    this->a_ = this->tail_;
    this->b_ = this->tail_;

    this->midSum_ = this->backSum_;
    this->backSum_ = this->identity_;
  }

  // DABA Lite algorithm methods
  inline bool is_front_empty() { return this->b_ == this->head_; }
  inline bool is_delta_empty() { return this->a_ == this->b_; }
  inline Partial get_back_() { return this->backSum_; }
  inline Partial get_alpha_() { return is_front_empty() ? this->identity_ : this->q_[this->head_]; }
  inline Partial get_delta_() { return is_delta_empty() ? this->identity_ : this->q_[this->a_]; }  
};

}  // namespace statistics
}  // namespace esphome
