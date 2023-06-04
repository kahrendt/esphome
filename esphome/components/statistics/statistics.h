#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include<limits>
// #include "DABALite.hpp"
// #include "AggregationFunctions.hpp"

namespace esphome {
namespace statistics {

class StatisticsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void setup() override;

  void set_source_sensor(sensor::Sensor *source_sensor) { this->source_sensor_ = source_sensor; }

  void set_mean_sensor(sensor::Sensor *mean_sensor) { this->mean_sensor_ = mean_sensor; }
  void set_max_sensor(sensor::Sensor *max_sensor) { this->max_sensor_ = max_sensor; }
  void set_min_sensor(sensor::Sensor *min_sensor) { this->min_sensor_ = min_sensor; }
  void set_sd_sensor(sensor::Sensor *sd_sensor) { this->sd_sensor_ = sd_sensor; }

  void set_window_size(size_t window_size) { this->window_size_ = window_size; }
  void set_send_every(size_t send_every) { this->send_every_ = send_every; }
  void set_first_at(size_t send_first_at) { this->send_at_ = send_first_at; }

 protected:
  sensor::Sensor *source_sensor_{nullptr};

  void handle_new_value_(float value);

  sensor::Sensor *mean_sensor_{nullptr};
  sensor::Sensor *max_sensor_{nullptr};  
  sensor::Sensor *min_sensor_{nullptr};  
  sensor::Sensor *sd_sensor_{nullptr};

  std::vector<float> queue_;
  size_t index_ = 0;

  size_t update_count_ = 0;
  size_t valid_count_ = 0;
  float sum_ = 0;

  float mean_ = 0;
  float m2_ = 0;

  size_t window_size_;
  size_t send_every_;
  size_t send_at_;
// };

// class Aggregate {
//   public:
    struct Partial {
      float sum;
      float sq;
      float max;
      float min;
      size_t count;
    };

    float lower_mean(Partial c) {
      return static_cast<float>(c.sum/c.count);
    }

    float lower_max(Partial c) {
      return static_cast<float>(c.max);
    }

    float lower_min(Partial c) {
      return static_cast<float>(c.min);
    }

    float lower_sd(Partial c) {
      return static_cast<float>(std::sqrt((1.0 / (c.count - 1)) * (c.sq - ((c.sum * c.sum) / static_cast<double>(c.count)))));
    }

    Partial lift(float v) {
      Partial part;
      part.sum = v;
      part.sq = v * v;
      part.max = v;
      part.min = v;
      part.count = 1;
      return part;
    }

    Partial combine(Partial &a, Partial &b) {
      Partial part;
      part.sum = a.sum + b.sum;
      part.sq = a.sq + b.sq;
      part.count = a.count + b.count;

      part.max = std::max(a.max, b.max);
      part.min = std::min(a.min, b.min);

      return part;
    }

  
    size_t size() { return q_.size(); }

    void insert(float value) {
      Partial lifted = lift(value);
      backSum_ = combine(backSum_, lifted);

      q_.push_back(lifted);      

      step_();
    }

    void evict() {
      q_.pop_front();
      step_();
    }

    Partial query() {
      if (q_.size() > 0) {
        Partial alpha = get_alpha_();
        Partial back = get_back_();

        return combine(alpha, back);
      }
      else
        return identity_;
    }

    // float query() {
    //   if (q_.size() > 0) {
    //     Partial alpha = get_alpha_();
    //     Partial back = get_back_();

    //     return lower(combine(alpha, back));
    //   }
    //   else
    //     return lower(identity_);
    // }

  private:
    std::deque<Partial> q_;
    std::deque<Partial>::iterator l_,r_,a_,b_;

    Partial identity_ = {0,0,std::numeric_limits<float>::infinity()*(-1),std::numeric_limits<float>::infinity(),0};

    Partial midSum_, backSum_;

    void step_() {
      if (l_ == b_) {
        flip_();
      }

      if (q_.begin() != b_) {
        if (a_ != r_) {
          Partial prev_delta = get_delta_();
          --a_;
          *a_ = combine(*a_, prev_delta);
        }

        if (l_ != r_) {
          *l_ = combine(*l_, midSum_);
          ++l_;
        }
        else {
          ++l_; ++r_; ++a_;
          midSum_ = get_delta_();
        }
      }
      else {
        backSum_ = midSum_ = identity_;
      }
    }

    void flip_() {
      l_ = q_.begin();
      r_ = b_;
      a_ = q_.end();
      b_ = q_.end();
      midSum_ = backSum_;
      backSum_ = identity_;
    }

    inline bool is_back_empty() { return b_ == q_.end(); }
    inline bool is_front_empty() { return b_ == q_.begin(); }
    inline bool is_delta_empty() { return a_ == b_; }
    inline bool is_gamma_empty() { return a_ == r_; }
    inline Partial get_back_() { return backSum_; }
    inline Partial get_alpha_() { return is_front_empty() ? identity_ : q_.front(); }
    inline Partial get_delta_() { return is_delta_empty() ? identity_ : *a_; }
    inline Partial get_gamma_() { return is_gamma_empty() ? identity_ : *(a_-1); }    
};

}  // namespace statistics
}  // namespace esphome
