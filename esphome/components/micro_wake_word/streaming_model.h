#pragma once

/**
 * This is a workaround until we can figure out a way to get
 * the tflite-micro idf component code available in CI
 *
 * */
//
#ifndef CLANG_TIDY

#ifdef USE_ESP_IDF

#include "preprocessor_settings.h"

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

namespace esphome {
namespace micro_wake_word {

static const uint32_t STREAMING_MODEL_ARENA_SIZE = 45672;
static const uint32_t STREAMING_MODEL_VARIABLE_ARENA_SIZE = 1024;

class StreamingModel {
 public:
  StreamingModel(const uint8_t *model_start, float probability_cutoff, size_t sliding_window_average_size,
                 const std::string &wake_word) {
    this->model_start_ = model_start;
    this->probability_cutoff_ = probability_cutoff;
    this->sliding_window_average_size_ = sliding_window_average_size;
    this->recent_streaming_probabilities_.resize(sliding_window_average_size, 0.0);
    this->wake_word_ = wake_word;
  };

  void log_model_config();

  std::string get_wake_word() { return this->wake_word_; }
  bool perform_streaming_inference(const int8_t features[PREPROCESSOR_FEATURE_SIZE]);
  void reset_probabilities();

  bool initialize_model(tflite::MicroMutableOpResolver<17> &op_resolver);
  bool wake_word_detected();

 protected:
  const uint8_t *model_start_;
  float probability_cutoff_;
  size_t sliding_window_average_size_;
  size_t last_n_index_{0};
  uint8_t *tensor_arena_{nullptr};
  uint8_t *var_arena_{nullptr};
  const tflite::Model *model_{nullptr};
  tflite::MicroInterpreter *interpreter_{nullptr};
  tflite::MicroResourceVariables *mrv_{nullptr};
  tflite::MicroAllocator *ma_{nullptr};
  std::string wake_word_;
  std::vector<float> recent_streaming_probabilities_;
};
}  // namespace micro_wake_word
}  // namespace esphome

#endif
#endif
