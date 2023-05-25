/*
 * 
 */

#include "statistics.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace statistics {

static const char *const TAG = "statistics";


void StatisticsComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Statistics:");

  ESP_LOGCONFIG(TAG, "  window_size: %u", this->window_size_);
  ESP_LOGCONFIG(TAG, "  send_every: %u", this->send_every_);
  ESP_LOGCONFIG(TAG, "  send_first_at: %u", this->send_at_);

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Mean", this->mean_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Min", this->min_sensor_);
  }

  if (this->mean_sensor_) {
    LOG_SENSOR("  ", "Max", this->max_sensor_);
  }
}

void StatisticsComponent::setup() {
  // this->queue_.reserve(this->window_size_);
  // fill(this->queue_.begin(), this->queue_.end(), NAN);
  
  this->queue_.resize(this->window_size_, NAN);
  this->queue_.shrink_to_fit();

  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}


void StatisticsComponent::handle_new_value_(float value) {


  // update running sum with new value
  if (this->mean_sensor_) {

    // check if old value is valid, if so, remove from running sum
    float old_value = this->queue_[this->index_];
    if (!std::isnan(old_value)) {
      this->sum_ -= old_value;
    }

    // if new value is valid, add it to running sum
    if (!std::isnan(value)) {
      this->sum_ += value;
    }

    // old value is NaN, but new value is valid
    if ( (std::isnan(old_value)) && (!std::isnan(value)) ) { 
      ++this->valid_count_;
    }

    if ( (!std::isnan(old_value)) && (std::isnan(value)) )  { // old value was good, but new one isn't
      this->valid_count_ = std::max(0, (int)(this->valid_count_ - 1) );
    }
  }

  // overwrite old value in queue_ and increase the index
  this->queue_[this->index_] = value;
  this->index_ = (1 + this->index_) % this->window_size_;

  // while (this->queue_.size() >= this->window_size_) {
  //   // float remove_value = this->queue_[0];
  //   // if (!std::isnan(remove_value)) {
  //   //   this->sum_ -= remove_value;
  //   //   --this->valid_count_;
  //   // }
  //   this->queue_.pop_front();
  // }
  
  // this->queue_.push_back(value);
  // // if (!std::isnan(value)) {
  // //   this->sum_ += value;
  // //   ++this->valid_count_;
  // // }

  if (++this->send_at_ >= this->send_every_) {
    this->send_at_ = 0;



    // float sum = 0;
    // size_t valid_count = 0;

    // for (auto v : this->queue_) {
    //   if (!std::isnan(v)) {
    //     //max = std::isnan(max) ? v : std::max(max, v);
    //     //min = std::isnan(min) ? v : std::min(min, v);
    //     sum += v;
    //     ++valid_count;
    //   }
    // }


    // if (valid_count) {
    //   average = sum/valid_count;
    // }

    if (this->mean_sensor_) {
      float average = NAN;
      
      if (this->valid_count_) {
        average = this->sum_ / this->valid_count_;
      }

      this->mean_sensor_->publish_state(average);
    }

    if (this->max_sensor_) {
      float max = *std::max_element(this->queue_.begin(), this->queue_.end());

      this->max_sensor_->publish_state(max);
    }
    if (this->min_sensor_) {
      float min = *std::min_element(this->queue_.begin(), this->queue_.end());

      this->min_sensor_->publish_state(min);
    }    
  }
}

}  // namespace statistics
}  // namespace esphome
