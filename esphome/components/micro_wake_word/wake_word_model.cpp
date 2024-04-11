#include "wake_word_model.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

static const char *const TAG = "micro_wake_word";

namespace esphome {
namespace micro_wake_word {

void WakeWordModel::log_model_config() {
  ESP_LOGCONFIG(TAG, "  - Wake Word: %s", this->wake_word_.c_str());
  ESP_LOGCONFIG(TAG, "    Probability cutoff: %.3f", this->probability_cutoff_);
  ESP_LOGCONFIG(TAG, "    Sliding window size: %d", this->sliding_window_average_size_);
}

bool WakeWordModel::initialize_model(tflite::MicroMutableOpResolver<17> &op_resolver) {
  ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);

  this->tensor_arena_ = arena_allocator.allocate(STREAMING_MODEL_ARENA_SIZE);
  if (this->tensor_arena_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the streaming model's tensor arena.");
    return false;
  }

  this->var_arena_ = arena_allocator.allocate(STREAMING_MODEL_VARIABLE_ARENA_SIZE);
  if (this->var_arena_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the streaming model variable's tensor arena.");
    return false;
  }

  this->model_ = tflite::GetModel(this->model_start_);
  if (this->model_->version() != TFLITE_SCHEMA_VERSION) {
    ESP_LOGE(TAG, "Wake word's streaming model's schema is not supported");
    return false;
  }
  this->ma_ = tflite::MicroAllocator::Create(this->var_arena_, STREAMING_MODEL_VARIABLE_ARENA_SIZE);
  this->mrv_ = tflite::MicroResourceVariables::Create(this->ma_, 15);

  this->interpreter_ = new tflite::MicroInterpreter(this->model_, op_resolver, this->tensor_arena_,
                                                    STREAMING_MODEL_ARENA_SIZE, this->mrv_);

  if (this->interpreter_->AllocateTensors() != kTfLiteOk) {
    ESP_LOGE(TAG, "Failed to allocate tensors for the streaming model");
    return false;
  }

  // Verify input tensor matches expected values
  TfLiteTensor *input = this->interpreter_->input(0);
  if ((input->dims->size != 3) || (input->dims->data[0] != 1) || (input->dims->data[0] != 1) ||
      (input->dims->data[1] != 1) || (input->dims->data[2] != PREPROCESSOR_FEATURE_SIZE)) {
    ESP_LOGE(TAG, "Wake word detection model tensor input dimensions is not 1x1x%u", input->dims->data[2]);
    return false;
  }

  if (input->type != kTfLiteInt8) {
    ESP_LOGE(TAG, "Wake word detection model tensor input is not int8.");
    return false;
  }

  // Verify output tensor matches expected values
  TfLiteTensor *output = this->interpreter_->output(0);
  if ((output->dims->size != 2) || (output->dims->data[0] != 1) || (output->dims->data[1] != 1)) {
    ESP_LOGE(TAG, "Wake word detection model tensor output dimension is not 1x1.");
  }

  if (output->type != kTfLiteUInt8) {
    ESP_LOGE(TAG, "Wake word detection model tensor output is not uint8.");
    return false;
  }

  return true;
}

bool WakeWordModel::perform_streaming_inference(int8_t features[PREPROCESSOR_FEATURE_SIZE]) {
  TfLiteTensor *input = this->interpreter_->input(0);

  size_t bytes_to_copy = input->bytes;

  memcpy((void *) (tflite::GetTensorData<int8_t>(input)), (const void *) (features), bytes_to_copy);

  uint32_t prior_invoke = millis();

  TfLiteStatus invoke_status = this->interpreter_->Invoke();
  if (invoke_status != kTfLiteOk) {
    ESP_LOGW(TAG, "Streaming Interpreter Invoke failed");
    return false;
  }

  ESP_LOGV(TAG, "Streaming Inference Latency=%u ms", (millis() - prior_invoke));

  TfLiteTensor *output = this->interpreter_->output(0);

  float probability = static_cast<float>(output->data.uint8[0]) / 255.0;
  ++this->last_n_index_;
  if (this->last_n_index_ == this->sliding_window_average_size_)
    this->last_n_index_ = 0;
  this->recent_streaming_probabilities_[this->last_n_index_] = probability;

  return true;
}

void WakeWordModel::reset_probabilities() {
  for (auto &prob : this->recent_streaming_probabilities_) {
    prob = 0.0;
  }
}

bool WakeWordModel::wake_word_detected() {
  float sum = 0.0;
  for (auto &prob : this->recent_streaming_probabilities_) {
    sum += prob;
  }

  float sliding_window_average = sum / static_cast<float>(this->sliding_window_average_size_);

  // Detect the wake word if the sliding window average is above the cutoff
  if (sliding_window_average > this->probability_cutoff_) {
    // ESP_LOGD(TAG, "Wake word sliding average probability is %.3f and most recent probability is %.3f",
    //          sliding_window_average, streaming_prob);
    return true;
  }
  return false;
}

}  // namespace micro_wake_word
}  // namespace esphome
