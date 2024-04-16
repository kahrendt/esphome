#pragma once

/**
 * This is a workaround until we can figure out a way to get
 * the tflite-micro idf component code available in CI
 *
 * */
//
#ifndef CLANG_TIDY

#ifdef USE_ESP_IDF

#define MWW_TIMING_DEBUG

#include "preprocessor_settings.h"
#include "streaming_model.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include "esphome/components/microphone/microphone.h"

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

namespace esphome {
namespace micro_wake_word {

enum State {
  IDLE,
  START_MICROPHONE,
  STARTING_MICROPHONE,
  DETECTING_WAKE_WORD,
  STOP_MICROPHONE,
  STOPPING_MICROPHONE,
};

// The number of audio slices to process before accepting a positive detection
static const uint8_t MIN_SLICES_BEFORE_DETECTION = 74;

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
#ifdef MWW_TIMING_DEBUG
  size_t window_counter_{0};
  size_t millis_start_of_counter_{0};
#endif

  void set_state_(State state);
  int read_microphone_();

  microphone::Microphone *microphone_{nullptr};
  Trigger<std::string> *wake_word_detected_trigger_ = new Trigger<std::string>();
  State state_{State::IDLE};
  HighFrequencyLoopRequester high_freq_;

  std::unique_ptr<RingBuffer> ring_buffer_;

  std::vector<StreamingModel *> streaming_models_;

  tflite::MicroInterpreter *preprocessor_interperter_{nullptr};

  // When the wake word detection first starts or after the word has been detected once, we ignore this many audio
  // feature slices before accepting a positive detection again
  int16_t ignore_windows_{-MIN_SLICES_BEFORE_DETECTION};

  uint8_t *preprocessor_tensor_arena_{nullptr};

  int16_t *input_buffer_{nullptr};
  // Stores audio fed into feature generator preprocessor. Also used for striding samples in each window
  int16_t *preprocessor_audio_buffer_{nullptr};

  bool detected_{false};
  std::string detected_wake_word_{""};

  /** Performs inference with each configured model
   *
   * If enough audio samples are available, it will generate one slice of new features.
   * It then loops through and performs inference with each of the loaded models.
   */
  void update_model_probabilities_();

  /** Checks every model's recent probabilities to determine if the wake word has been predicted
   *
   * Verifies the models have processed enough new samples for accurate predictions.
   * Sets detected_wake_word_ to the wake word, if one is detected.
   * @return True if a wake word is predicted, false otherwise
   */
  bool detect_independent_wake_words_();

  /** Checks every model's recent probabilities to determine if every model predicts the wake word
   *
   * Verifies the models have processed enough new samples for accurate predictions.
   * Sets detected_wake_word_ to the wake word, if every model predicts it.
   * @return True if a wake word is predicted, false otherwise
   */
  bool detect_ensemble_wake_word_();

  /** Reads in new audio data from ring buffer to create the next sample window
   *
   * Moves the last 10 ms of audio from the previous window to the start of the new window.
   * The next 20 ms of audio is copied from the ring buffer and inserted into the new window.
   * The new window's audio samples are stored in preprocessor_audio_buffer_.
   * Adapted from the TFLite micro speech example.
   * @return True if successful, false otherwise
   */
  bool stride_audio_samples_();

  /** Generates features for a window of audio samples
   *
   * Feeds the strided audio samples in preprocessor_audio_buffer_ into the preprocessor.
   * Adapted from TFLite micro speech example.
   * @param features int8_t array to store the audio features
   * @return True if successful, false otherwise.
   */
  bool generate_features_for_window_(int8_t features[PREPROCESSOR_FEATURE_SIZE]);

  /// @brief Resets the ring buffer, ignore_windows_, and sliding window probabilities
  void reset_states_();

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
