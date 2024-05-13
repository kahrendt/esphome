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
// #include <tensorflow/lite/micro/kernels/esp_nn/conv.cc>

extern long long conv_total_time;
extern long long dc_total_time;
extern long long pooling_total_time;
extern long long add_total_time;
extern long long fc_total_time;

namespace esphome {
namespace micro_wake_word {

static const uint32_t STREAMING_MODEL_VARIABLE_ARENA_SIZE = 2048;

// long long total_time = 0;
// long long start_time = 0;
// extern long long softmax_total_time;
// extern long long dc_total_time;

// extern long long fc_total_time;
// extern long long pooling_total_time;
// extern long long add_total_time;
// extern long long mul_total_time;

class StreamingModel {
 public:
  virtual void log_model_config() = 0;
  virtual bool determine_detected() = 0;

  bool perform_streaming_inference(const int8_t features[PREPROCESSOR_FEATURE_SIZE]);

  /// @brief Sets all recent_streaming_probabilities to 0
  void reset_probabilities();

  /// @brief Allocates tensor and variable arenas and sets up the model interpreter
  /// @param op_resolver MicroMutableOpResolver object that must exist until the model is unloaded
  /// @return True if successful, false otherwise
  bool load_model(tflite::MicroMutableOpResolver<20> &op_resolver);

  /// @brief Destroys the TFLite interpreter and frees the tensor and variable arenas' memory
  void unload_model();

 protected:
  size_t sliding_window_size_;
  size_t last_n_index_{0};
  size_t tensor_arena_size_;
  std::vector<uint8_t> recent_streaming_probabilities_;

  const uint8_t *model_start_;
  uint8_t *tensor_arena_{nullptr};
  uint8_t *var_arena_{nullptr};
  tflite::MicroInterpreter *interpreter_{nullptr};
  tflite::MicroResourceVariables *mrv_{nullptr};
  tflite::MicroAllocator *ma_{nullptr};
};

class WakeWordModel : public StreamingModel {
 public:
  WakeWordModel(const uint8_t *model_start, float probability_cutoff, size_t sliding_window_average_size,
                const std::string &wake_word, size_t tensor_arena_size);

  void log_model_config() override;
  bool determine_detected() override;

  std::string get_wake_word() { return this->wake_word_; }

 protected:
  float probability_cutoff_;
  std::string wake_word_;
};

class VADModel : public StreamingModel {
 public:
  VADModel(const uint8_t *model_start, float upper_threshold, float lower_threshold, size_t sliding_window_size,
           size_t tensor_arena_size);

  void log_model_config() override;
  bool determine_detected() override;

 protected:
  uint8_t clear_countdown_{10};
  bool vad_state_{false};
  float upper_threshold_;
  float lower_threshold_;
};

}  // namespace micro_wake_word
}  // namespace esphome

#endif
#endif
