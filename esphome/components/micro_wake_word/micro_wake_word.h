#pragma once

/**
 * This is a workaround until we can figure out a way to get
 * the tflite-micro idf component code available in CI
 *
 * */
//
#ifndef CLANG_TIDY

#ifdef USE_ESP_IDF

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include "esphome/components/microphone/microphone.h"

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

namespace esphome {
namespace micro_wake_word {

// The following are dictated by the preprocessor model
//
// The number of features the audio preprocessor generates per slice
static const uint8_t PREPROCESSOR_FEATURE_SIZE = 40;
// How frequently the preprocessor generates a new set of features
static const uint8_t FEATURE_STRIDE_MS = 20;
// Duration of each slice used as input into the preprocessor
static const uint8_t FEATURE_DURATION_MS = 30;
// Audio sample frequency in hertz
static const uint16_t AUDIO_SAMPLE_FREQUENCY = 16000;
// The number of old audio samples that are saved to be part of the next feature window
static const uint16_t HISTORY_SAMPLES_TO_KEEP =
    ((FEATURE_DURATION_MS - FEATURE_STRIDE_MS) * (AUDIO_SAMPLE_FREQUENCY / 1000));
// The number of new audio samples to receive to be included with the next feature window
static const uint16_t NEW_SAMPLES_TO_GET = (FEATURE_STRIDE_MS * (AUDIO_SAMPLE_FREQUENCY / 1000));
// The total number of audio samples included in the feature window
static const uint16_t SAMPLE_DURATION_COUNT = FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQUENCY / 1000;
// Number of bytes in memory needed for the preprocessor arena
static const uint32_t PREPROCESSOR_ARENA_SIZE = 9528;

// The following configure the streaming wake word model
//
// The number of audio slices to process before accepting a positive detection
static const uint8_t MIN_SLICES_BEFORE_DETECTION = 74;

// Number of bytes in memory needed for the streaming wake word model
static const uint32_t STREAMING_MODEL_ARENA_SIZE = 48000;
static const uint32_t STREAMING_MODEL_VARIABLE_ARENA_SIZE = 1024;

enum State {
  IDLE,
  START_MICROPHONE,
  STARTING_MICROPHONE,
  DETECTING_WAKE_WORD,
  STOP_MICROPHONE,
  STOPPING_MICROPHONE,
};

struct WakeWordModel {
  const uint8_t *model_start;
  float probability_cutoff;
  size_t sliding_window_average_size;
  size_t last_n_index{0};
  uint8_t *tensor_arena{nullptr};
  uint8_t *var_arena{nullptr};
  const tflite::Model *streaming_model{nullptr};
  tflite::MicroInterpreter *interpreter{nullptr};
  tflite::MicroResourceVariables *mrv{nullptr};
  tflite::MicroAllocator *ma{nullptr};
  std::string wake_word;
  std::vector<float> recent_streaming_probabilities;
};

class MicroWakeWord : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;
  void dump_config() override;

  void start();
  void stop();

  bool is_running() const { return this->state_ != State::IDLE; }

  bool initialize_models();

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }

  Trigger<std::string> *get_wake_word_detected_trigger() const { return this->wake_word_detected_trigger_; }

  void add_model(const uint8_t *model_start, float probability_cutoff, size_t sliding_window_average_size,
                 const std::string &wake_word);

 protected:
  void set_state_(State state);
  int read_microphone_();

  microphone::Microphone *microphone_{nullptr};
  Trigger<std::string> *wake_word_detected_trigger_ = new Trigger<std::string>();
  State state_{State::IDLE};
  HighFrequencyLoopRequester high_freq_;

  std::unique_ptr<RingBuffer> ring_buffer_;

  int16_t *input_buffer_;

  std::vector<WakeWordModel> wake_word_models_;

  const tflite::Model *preprocessor_model_{nullptr};
  tflite::MicroInterpreter *preprocessor_interperter_{nullptr};

  // When the wake word detection first starts or after the word has been detected once, we ignore this many audio
  // feature slices before accepting a positive detection again
  int16_t ignore_windows_{-MIN_SLICES_BEFORE_DETECTION};

  uint8_t *preprocessor_tensor_arena_{nullptr};
  int8_t *new_features_data_{nullptr};

  // Stores audio fed into feature generator preprocessor and used for striding samples in each window
  int16_t *preprocessor_audio_buffer_;

  bool detected_{false};
  std::string *detected_wake_word_{nullptr};

  /** Detects if a wake word has been said
   *
   * If enough audio samples are available, it will generate one slice of new features.
   * It then loops through and performs inference with each of the loaded models.
   * @return True if a wake word is detected, false otherwise
   */
  bool detect_wake_word_();

  /// @brief Returns true if there are enough audio samples in the ring buffer to generate a slice of features
  bool slice_available_();

  /** Strides the audio window samples and computes/stores new features in this->new_features_data_
   *
   * @return True if a new slice of features was generated, false otherwise
   */
  bool update_features_();

  /** Generates features from audio samples
   *
   * Adapted from TFLite micro speech example
   * @param audio_data Pointer to array with strided audio samples
   * @param audio_data_size The number of samples to use as input to the preprocessor model
   * @param feature_output Array that will store the features
   * @return True if successful, false otherwise.
   */
  bool generate_single_feature_(const int16_t *audio_data, int audio_data_size,
                                int8_t feature_output[PREPROCESSOR_FEATURE_SIZE]);

  /** Performs inference over the most recent features slice with the specified model
   *
   * @param model WakeWordModel struct to infer with
   * @return Probability of the wake word between 0.0 and 1.0
   */
  float perform_streaming_inference_(WakeWordModel model);

  /** Strides the audio samples by keeping the last 10 ms of the previous window
   *
   * Adapted from the TFLite micro speech example
   * @param audio_samples Pointer to an array that will store the strided audio samples
   * @return True if successful, false otherwise
   */
  bool stride_audio_samples_(int16_t **audio_samples);

  /// @brief Returns true if successfully registered the preprocessor's TensorFlow operations
  bool register_preprocessor_ops_(tflite::MicroMutableOpResolver<18> &op_resolver);

  /// @brief Returns true if successfully registered the streaming model's TensorFlow operations
  bool register_streaming_ops_(tflite::MicroMutableOpResolver<17> &op_resolver);
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<MicroWakeWord> {
 public:
  void play(Ts... x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<MicroWakeWord> {
 public:
  void play(Ts... x) override { this->parent_->stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<MicroWakeWord> {
 public:
  bool check(Ts... x) override { return this->parent_->is_running(); }
};

}  // namespace micro_wake_word
}  // namespace esphome

#endif  // USE_ESP_IDF

#endif  // CLANG_TIDY
