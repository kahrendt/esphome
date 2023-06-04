/*
 * 
 */

#include "statistics.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <numeric>
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
      l_ = q_.begin();
      r_ = q_.begin();
      a_ = q_.begin();
      b_ = q_.begin();

  // auto daba_lite_agg = dabalite::make_aggregate(Mean<int>(),0);
  
  this->queue_.resize(this->window_size_, NAN);
  this->queue_.shrink_to_fit();

  this->source_sensor_->add_on_state_callback([this](float value) -> void { this->handle_new_value_(value); });
}


void StatisticsComponent::handle_new_value_(float value) {


  // update running sum with new value
  if ((this->mean_sensor_) || (this->sd_sensor_)) {

    // check if old value is valid, if so, remove from running sum and decrease counter
    float old_value = this->queue_[this->index_];
    if (!std::isnan(old_value)) {
      // daba_lite_agg_.evict();

      evict();

      --this->valid_count_;
      this->sum_ -= old_value;

      float delta = old_value-this->mean_;
      this->mean_ -= delta/((float)this->valid_count_);
      float delta2 = old_value-this->mean_;

      this->m2_ -= delta*delta2;
    }

    // // old value is NaN, but new value is valid
    // if ( (std::isnan(old_value)) && (!std::isnan(value)) ) { 
    //   ++this->valid_count_;
    // }

    // if ( (!std::isnan(old_value)) && (std::isnan(value)) )  { // old value was good, but new one isn't
    //   this->valid_count_ = std::max(0, (int)(this->valid_count_ - 1) );
    // }

    // if new value is valid, add it to running sum
    if (!std::isnan(value)) {
      // daba_lite_agg_.insert(value);
      insert(value);


      ++this->valid_count_;
      this->sum_ += value;

      float delta = value - this->mean_;
      
      this->mean_ += delta/((float)this->valid_count_);

      float delta2 = value - this->mean_;

      this->m2_ += delta*delta2;
    }
    else
      ESP_LOGE(TAG, "read in NAN value");

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


    Partial summary = query();
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

      //this->mean_sensor_->publish_state(average);

      float naive = std::accumulate(this->queue_.begin(), this->queue_.end(), 0.0)/((float) this->valid_count_);
      ESP_LOGI(TAG, " valid count: %u", this->valid_count_);
      ESP_LOGI(TAG, "   naive mean old algorithm difference: %.5f", (naive-average));
      ESP_LOGI(TAG, "naive mean online algorithm difference: %.5f", (naive-this->mean_));
      ESP_LOGI(TAG, "  naive daba lite algorithm difference: %.5f", (naive-lower_mean(summary)));
      this->mean_sensor_->publish_state(average);
    }

    if (this->max_sensor_) {
      float max = *std::max_element(this->queue_.begin(), this->queue_.end());

      ESP_LOGI(TAG, " maximum info real: %.2f; new: %.2f", max, lower_max(summary));

      this->max_sensor_->publish_state(max);
    }
    if (this->min_sensor_) {
      float min = *std::min_element(this->queue_.begin(), this->queue_.end());

      ESP_LOGI(TAG, " minimum info min! real: %.2f; new: %.2f", min, lower_min(summary));     

      this->min_sensor_->publish_state(min);
    }

    if (this->sd_sensor_) {
      if (this->valid_count_ < 2)
        this->sd_sensor_->publish_state(NAN);
      else {
        float variance = this->m2_/(this->valid_count_-1);

      ESP_LOGI(TAG, " standard deviation, old: %.2f; new: %.2f", sqrt(variance), lower_sd(summary));        
        this->sd_sensor_->publish_state(sqrt(variance));
      }

  

    }
  }
}

}  // namespace statistics
}  // namespace esphome
