#include "micro_wake_word.h"
#include "streaming_model.h"

/**
 * This is a workaround until we can figure out a way to get
 * the tflite-micro idf component code available in CI
 *
 * */
//
#ifndef CLANG_TIDY

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include "audio_preprocessor_int8_model_data.h"

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

#include <cmath>

#include "esp32/clk.h"

namespace esphome {
namespace micro_wake_word {

static const char *const TAG = "micro_wake_word";

static const size_t SAMPLE_RATE_HZ = 16000;  // 16 kHz
static const size_t BUFFER_LENGTH = 100;     // 0.1 seconds
static const size_t BUFFER_SIZE = SAMPLE_RATE_HZ / 1000 * BUFFER_LENGTH;
static const size_t INPUT_BUFFER_SIZE = 32 * SAMPLE_RATE_HZ / 1000;  // 32ms * 16kHz / 1000ms

float MicroWakeWord::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

static const LogString *micro_wake_word_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return LOG_STR("IDLE");
    case State::START_MICROPHONE:
      return LOG_STR("START_MICROPHONE");
    case State::STARTING_MICROPHONE:
      return LOG_STR("STARTING_MICROPHONE");
    case State::DETECTING_WAKE_WORD:
      return LOG_STR("DETECTING_WAKE_WORD");
    case State::STOP_MICROPHONE:
      return LOG_STR("STOP_MICROPHONE");
    case State::STOPPING_MICROPHONE:
      return LOG_STR("STOPPING_MICROPHONE");
    default:
      return LOG_STR("UNKNOWN");
  }
}

void MicroWakeWord::dump_config() {
  ESP_LOGCONFIG(TAG, "microWakeWord models:");
  for (auto &model : this->wake_word_models_) {
    model.log_model_config();
  }
#ifdef USE_MWW_VAD
  this->vad_model_->log_model_config();
#endif
}

void MicroWakeWord::setup() {
  ESP_LOGCONFIG(TAG, "Setting up microWakeWord...");

  if (!this->register_streaming_ops_(this->streaming_op_resolver_)) {
    this->mark_failed();
    return;
  }

  if (!this->register_preprocessor_ops_(this->preprocessor_op_resolver_)) {
    this->mark_failed();
    return;
  }

  this->features_queue_ = xQueueCreate(10, sizeof(DataEvent));
  this->event_queue_ = xQueueCreate(10, sizeof(TaskEvent));

  ESP_LOGCONFIG(TAG, "Micro Wake Word initialized");
}

void MicroWakeWord::add_wake_word_model(const uint8_t *model_start, float probability_cutoff,
                                        size_t sliding_window_average_size, const std::string &wake_word,
                                        size_t tensor_arena_size) {
  this->wake_word_models_.push_back(
      WakeWordModel(model_start, probability_cutoff, sliding_window_average_size, wake_word, tensor_arena_size));
}

#ifdef USE_MWW_VAD
void MicroWakeWord::add_vad_model(const uint8_t *model_start, float upper_threshold, float lower_threshold,
                                  size_t sliding_window_size, size_t tensor_arena_size) {
  this->vad_model_ =
      new VADModel(model_start, upper_threshold, lower_threshold, sliding_window_size, tensor_arena_size);
}
#endif

void MicroWakeWord::loop() {
  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
      ESP_LOGD(TAG, "Starting Microphone");
      this->microphone_->start();
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    case State::STARTING_MICROPHONE:
      if (this->microphone_->is_running()) {
        this->set_state_(State::DETECTING_WAKE_WORD);
      }
      break;
    case State::DETECTING_WAKE_WORD:
      this->generate_features_();
      this->read_microphone_();
      this->watch_();
      break;
    case State::STOP_MICROPHONE:
      ESP_LOGD(TAG, "Stopping Microphone");
      this->microphone_->stop();
      this->set_state_(State::STOPPING_MICROPHONE);
      this->unload_models_();
      this->deallocate_buffers_();
      break;
    case State::STOPPING_MICROPHONE:
      if (this->microphone_->is_stopped()) {
        this->set_state_(State::IDLE);
        if (this->detected_) {
          this->wake_word_detected_trigger_->trigger(this->detected_wake_word_);
          this->detected_ = false;
          this->detected_wake_word_ = "";
        }
      }
      break;
  }
}

void MicroWakeWord::start() {
  ESP_LOGD(TAG, "cpu frequency: %d", esp_clk_cpu_freq());
  // App.set_loop_interval(25);

  if (!this->is_ready()) {
    ESP_LOGW(TAG, "Wake word detection can't start as the component hasn't been setup yet");
    return;
  }

  if (this->is_failed()) {
    ESP_LOGW(TAG, "Wake word component is marked as failed. Please check setup logs");
    return;
  }

  if (!this->load_models_() || !this->allocate_buffers_()) {
    ESP_LOGE(TAG, "Failed to load the wake word model(s) or allocate buffers");
    this->status_set_error();
  } else {
    this->status_clear_error();
  }

  if (this->status_has_error()) {
    ESP_LOGW(TAG, "Wake word component has an error. Please check logs");
    return;
  }

  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Wake word is already running");
    return;
  }

  // this->inference_task_handle_ = xTaskCreateStatic(MicroWakeWord::inference_task_, "inference_task", 4096,
  //  (void *) this, 1, this->xStack_, &this->xTaskBuffer_);
  //   xTaskCreatePinnedToCore(MicroWakeWord::inference_task_, "inference_task", 4096, (void *) this, 5,
  //                           &this->inference_task_handle_, 1);
  xTaskCreate(MicroWakeWord::inference_task_, "inference_task", 4096, (void *) this, 17, &this->inference_task_handle_);

  this->reset_states_();
  this->set_state_(State::START_MICROPHONE);
}

void MicroWakeWord::stop() {
  xQueueReset(this->features_queue_);
  DataEvent data_event;
  data_event.stop = true;
  xQueueSend(this->features_queue_, &data_event, 0);

  if (this->state_ == State::IDLE) {
    ESP_LOGW(TAG, "Wake word is already stopped");
    return;
  }
  if (this->state_ == State::STOPPING_MICROPHONE) {
    ESP_LOGW(TAG, "Wake word is already stopping");
    return;
  }
  // this->set_state_(State::STOP_MICROPHONE);
}

void MicroWakeWord::set_state_(State state) {
  ESP_LOGD(TAG, "State changed from %s to %s", LOG_STR_ARG(micro_wake_word_state_to_string(this->state_)),
           LOG_STR_ARG(micro_wake_word_state_to_string(state)));
  this->state_ = state;
}

size_t MicroWakeWord::read_microphone_() {
  size_t bytes_read = this->microphone_->read(this->input_buffer_, INPUT_BUFFER_SIZE * sizeof(int16_t));
  if (bytes_read == 0) {
    return 0;
  }

  size_t bytes_free = this->ring_buffer_->free();

  if (bytes_free < bytes_read) {
    ESP_LOGW(TAG,
             "Not enough free bytes in ring buffer to store incoming audio data (free bytes=%d, incoming bytes=%d). "
             "Resetting the ring buffer. Wake word detection accuracy will be reduced.",
             bytes_free, bytes_read);

    this->ring_buffer_->reset();
  }

  return this->ring_buffer_->write((void *) this->input_buffer_, bytes_read);
}

bool MicroWakeWord::allocate_buffers_() {
  ExternalRAMAllocator<int16_t> audio_samples_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);

  if (this->input_buffer_ == nullptr) {
    this->input_buffer_ = audio_samples_allocator.allocate(INPUT_BUFFER_SIZE * sizeof(int16_t));
    if (this->input_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate input buffer");
      return false;
    }
  }

  if (this->preprocessor_audio_buffer_ == nullptr) {
    this->preprocessor_audio_buffer_ = audio_samples_allocator.allocate(SAMPLE_DURATION_COUNT);
    if (this->preprocessor_audio_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate the audio preprocessor's buffer.");
      return false;
    }
  }

  if (this->ring_buffer_ == nullptr) {
    this->ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
    if (this->ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate ring buffer");
      return false;
    }
  }

  return true;
}

void MicroWakeWord::deallocate_buffers_() {
  ExternalRAMAllocator<int16_t> audio_samples_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  audio_samples_allocator.deallocate(this->input_buffer_, PREPROCESSOR_ARENA_SIZE);
  this->input_buffer_ = nullptr;
  audio_samples_allocator.deallocate(this->preprocessor_audio_buffer_, PREPROCESSOR_ARENA_SIZE);
  this->preprocessor_audio_buffer_ = nullptr;
}

bool MicroWakeWord::load_models_() {
  // Setup preprocesor feature generator
  if (this->preprocessor_tensor_arena_ == nullptr) {
    ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->preprocessor_tensor_arena_ = arena_allocator.allocate(PREPROCESSOR_ARENA_SIZE);
    if (this->preprocessor_tensor_arena_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate the audio preprocessor model's tensor arena.");
      return false;
    }
  }
  if (this->preprocessor_interpreter_ == nullptr) {
    this->preprocessor_interpreter_ = new tflite::MicroInterpreter(
        tflite::GetModel(G_AUDIO_PREPROCESSOR_INT8_TFLITE), this->preprocessor_op_resolver_,
        this->preprocessor_tensor_arena_, PREPROCESSOR_ARENA_SIZE);

    if (this->preprocessor_interpreter_->AllocateTensors() != kTfLiteOk) {
      ESP_LOGE(TAG, "Failed to allocate tensors for the audio preprocessor");
      return false;
    }
  }
  //   // Setup streaming models
  //   for (auto &model : this->wake_word_models_) {
  //     if (!model.load_model(this->streaming_op_resolver_)) {
  //       ESP_LOGE(TAG, "Failed to initialize a wake word model.");
  //       return false;
  //     }
  //   }
  // #ifdef USE_MWW_VAD
  //   if (!this->vad_model_->load_model(this->streaming_op_resolver_)) {
  //     ESP_LOGE(TAG, "Failed to initialize VAD model.");
  //     return false;
  //   }
  // #endif

  return true;
}

void MicroWakeWord::unload_models_() {
  delete (this->preprocessor_interpreter_);
  this->preprocessor_interpreter_ = nullptr;

  ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);

  arena_allocator.deallocate(this->preprocessor_tensor_arena_, PREPROCESSOR_ARENA_SIZE);
  this->preprocessor_tensor_arena_ = nullptr;

  //   for (auto &model : this->wake_word_models_) {
  //     model.unload_model();
  //   }
  // #ifdef USE_MWW_VAD
  //   this->vad_model_->unload_model();
  // #endif
}

void MicroWakeWord::watch_() {
  TaskEvent event;
  if (xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    switch (event.type) {
      case TaskEventType::STARTING:
        ESP_LOGD(TAG, "Starting wake word inference");
        break;
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started wake word inference");
        break;
      case TaskEventType::STOPPING:
        ESP_LOGD(TAG, "Stopping wake word inference");
        break;
      case TaskEventType::DETECTING:
        this->status_clear_warning();
        break;
      case TaskEventType::DETECTED:
        // Verify we have processed samples since the last positive detection
        if (this->ignore_windows_ == 0) {
          ESP_LOGD(TAG, "Wake Word '%s' Detected", event.wakeword);
          this->detected_ = true;
          this->detected_wake_word_ = event.wakeword;
          DataEvent data_event;
          data_event.stop = true;
          xQueueSendToFront(this->features_queue_, &data_event, 0);
        }
        break;
      case TaskEventType::STOPPED:
        vTaskDelete(this->inference_task_handle_);
        this->inference_task_handle_ = nullptr;
        xQueueReset(this->features_queue_);
        ESP_LOGD(TAG, "Stopped wake word inference");
        this->set_state_(State::STOP_MICROPHONE);
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error performing wake word inference: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
      case TaskEventType::ERROR:
        ESP_LOGW(TAG, "Error performing wake word inference");  // TODO handle this better...
        this->status_set_error();
        break;
    }
  }
}

void MicroWakeWord::generate_features_() {
  int8_t audio_features[PREPROCESSOR_FEATURE_SIZE];
  while (this->stride_audio_samples_()) {
    if (!this->generate_features_for_window_(audio_features)) {
      return;  // TODO: Better handle this?
    }
    // Increase the counter since the last positive detection
    this->ignore_windows_ = std::min(this->ignore_windows_ + 1, 0);

    DataEvent event;
    event.stop = false;
    memcpy(event.data, audio_features, PREPROCESSOR_FEATURE_SIZE);

    if (xQueueSend(this->features_queue_, &event, 0) != pdTRUE) {
      ESP_LOGW(TAG, "Features queue is full, some audio samples are lost.");
    }
  }
}

void MicroWakeWord::inference_task_(void *params) {
  MicroWakeWord *this_instance = (MicroWakeWord *) params;
  DataEvent data_event;

  TaskEvent event;
  event.type = TaskEventType::STARTING;
  xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);

  bool initialized = true;

  // Setup streaming models
  for (auto &model : this_instance->wake_word_models_) {
    if (!model.load_model(this_instance->streaming_op_resolver_)) {
      ESP_LOGE(TAG, "Failed to initialize a wake word model.");
      event.type = TaskEventType::ERROR;
      xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);
      initialized = false;
    }
  }
#ifdef USE_MWW_VAD
  if (!this_instance->vad_model_->load_model(this_instance->streaming_op_resolver_)) {
    ESP_LOGE(TAG, "Failed to initialize VAD model.");
    event.type = TaskEventType::ERROR;
    xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);
    initialized = false;
  }
#endif

  if (initialized) {
    event.type = TaskEventType::STARTED;
    xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);
  }

  size_t inference_count = 0;
  size_t total_inference_time = 0;
  while (initialized) {
    if (xQueueReceive(this_instance->features_queue_, &data_event, 10 / portTICK_PERIOD_MS) != pdTRUE) {
      continue;
    }

    if (data_event.stop) {
      xQueueReset(this_instance->features_queue_);
      break;
    }

    for (auto &model : this_instance->wake_word_models_) {
      size_t start_time = micros();
      model.perform_streaming_inference(data_event.data);
      total_inference_time += (micros() - start_time);
      ++inference_count;
      // ESP_LOGD(TAG, "inference time %d; count=%d", (micros() - start_time), inference_count);
    }

    if (inference_count >= 999) {
      ESP_LOGD(TAG, "average inference time %.1f", (static_cast<float>(total_inference_time) / inference_count));
      inference_count = 0;
      total_inference_time = 0;
    }

#ifdef USE_MWW_VAD
    this_instance->vad_model_->perform_streaming_inference(data_event.data);
    // this_instance->vad_model_->perform_streaming_inference(features_buffer);
#endif

#ifdef USE_MWW_VAD
    bool vad_state = this_instance->vad_model_->determine_detected();
#endif

    for (auto &model : this_instance->wake_word_models_) {
      if (model.determine_detected()) {
#ifdef USE_MWW_VAD
        if (vad_state) {
#endif
          event.type = TaskEventType::DETECTED;
          std::string wakeword = model.get_wake_word();
          std::strcpy(event.wakeword, wakeword.c_str());
          xQueueSendToFront(this_instance->event_queue_, &event, portMAX_DELAY);
#ifdef USE_MWW_VAD
        } else {
          ESP_LOGD(TAG, "Wake word model predicts %s, but VAD model doesn't.", model.get_wake_word().c_str());
        }
#endif
      }
    }

    event.type = TaskEventType::DETECTING;
    xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);
  }

  event.type = TaskEventType::STOPPING;
  xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);

  for (auto &model : this_instance->wake_word_models_) {
    model.unload_model();
  }
#ifdef USE_MWW_VAD
  this_instance->vad_model_->unload_model();
#endif

  event.type = TaskEventType::STOPPED;
  xQueueSend(this_instance->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

bool MicroWakeWord::stride_audio_samples_() {
  // Ensure we have enough new audio samples in the ring buffer for a full window
  if (this->ring_buffer_->available() < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
    return false;
  }

  // Copy the last 320 bytes (160 samples over 10 ms) from the audio buffer to the start of the audio buffer
  memcpy((void *) (this->preprocessor_audio_buffer_), (void *) (this->preprocessor_audio_buffer_ + NEW_SAMPLES_TO_GET),
         HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

  // Copy 640 bytes (320 samples over 20 ms) from the ring buffer into the audio buffer offset 320 bytes (160 samples
  // over 10 ms)
  size_t bytes_read = this->ring_buffer_->read((void *) (this->preprocessor_audio_buffer_ + HISTORY_SAMPLES_TO_KEEP),
                                               NEW_SAMPLES_TO_GET * sizeof(int16_t), pdMS_TO_TICKS(200));

  if (bytes_read == 0) {
    ESP_LOGE(TAG, "Could not read data from Ring Buffer");
  } else if (bytes_read < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
    ESP_LOGD(TAG, "Partial Read of Data by Model");
    ESP_LOGD(TAG, "Could only read %d bytes when required %d bytes ", bytes_read,
             (int) (NEW_SAMPLES_TO_GET * sizeof(int16_t)));
    return false;
  }

  return true;
}

bool MicroWakeWord::generate_features_for_window_(int8_t features[PREPROCESSOR_FEATURE_SIZE]) {
  TfLiteTensor *input = this->preprocessor_interpreter_->input(0);
  TfLiteTensor *output = this->preprocessor_interpreter_->output(0);
  std::copy_n(this->preprocessor_audio_buffer_, SAMPLE_DURATION_COUNT, tflite::GetTensorData<int16_t>(input));

  if (this->preprocessor_interpreter_->Invoke() != kTfLiteOk) {
    ESP_LOGE(TAG, "Failed to preprocess audio for local wake word.");
    return false;
  }
  std::memcpy(features, tflite::GetTensorData<int8_t>(output), PREPROCESSOR_FEATURE_SIZE * sizeof(int8_t));

  return true;
}

void MicroWakeWord::reset_states_() {
  ESP_LOGD(TAG, "Resetting buffers and probabilities");
  this->ring_buffer_->reset();
  this->ignore_windows_ = -MIN_SLICES_BEFORE_DETECTION;
  for (auto &model : this->wake_word_models_) {
    model.reset_probabilities();
  }
#ifdef USE_MWW_VAD
  this->vad_model_->reset_probabilities();
#endif
}

bool MicroWakeWord::register_preprocessor_ops_(tflite::MicroMutableOpResolver<18> &op_resolver) {
  if (op_resolver.AddReshape() != kTfLiteOk)
    return false;
  if (op_resolver.AddCast() != kTfLiteOk)
    return false;
  if (op_resolver.AddStridedSlice() != kTfLiteOk)
    return false;
  if (op_resolver.AddConcatenation() != kTfLiteOk)
    return false;
  if (op_resolver.AddMul() != kTfLiteOk)
    return false;
  if (op_resolver.AddAdd() != kTfLiteOk)
    return false;
  if (op_resolver.AddDiv() != kTfLiteOk)
    return false;
  if (op_resolver.AddMinimum() != kTfLiteOk)
    return false;
  if (op_resolver.AddMaximum() != kTfLiteOk)
    return false;
  if (op_resolver.AddWindow() != kTfLiteOk)
    return false;
  if (op_resolver.AddFftAutoScale() != kTfLiteOk)
    return false;
  if (op_resolver.AddRfft() != kTfLiteOk)
    return false;
  if (op_resolver.AddEnergy() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBank() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankSquareRoot() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankSpectralSubtraction() != kTfLiteOk)
    return false;
  if (op_resolver.AddPCAN() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankLog() != kTfLiteOk)
    return false;

  return true;
}

bool MicroWakeWord::register_streaming_ops_(tflite::MicroMutableOpResolver<20> &op_resolver) {
  if (op_resolver.AddCallOnce() != kTfLiteOk)
    return false;
  if (op_resolver.AddVarHandle() != kTfLiteOk)
    return false;
  if (op_resolver.AddReshape() != kTfLiteOk)
    return false;
  if (op_resolver.AddReadVariable() != kTfLiteOk)
    return false;
  if (op_resolver.AddStridedSlice() != kTfLiteOk)
    return false;
  if (op_resolver.AddConcatenation() != kTfLiteOk)
    return false;
  if (op_resolver.AddAssignVariable() != kTfLiteOk)
    return false;
  if (op_resolver.AddConv2D() != kTfLiteOk)
    return false;
  if (op_resolver.AddMul() != kTfLiteOk)
    return false;
  if (op_resolver.AddAdd() != kTfLiteOk)
    return false;
  if (op_resolver.AddMean() != kTfLiteOk)
    return false;
  if (op_resolver.AddFullyConnected() != kTfLiteOk)
    return false;
  if (op_resolver.AddLogistic() != kTfLiteOk)
    return false;
  if (op_resolver.AddQuantize() != kTfLiteOk)
    return false;
  if (op_resolver.AddDepthwiseConv2D() != kTfLiteOk)
    return false;
  if (op_resolver.AddAveragePool2D() != kTfLiteOk)
    return false;
  if (op_resolver.AddMaxPool2D() != kTfLiteOk)
    return false;
  if (op_resolver.AddPad() != kTfLiteOk)
    return false;
  if (op_resolver.AddPack() != kTfLiteOk)
    return false;
  if (op_resolver.AddSplitV() != kTfLiteOk)
    return false;

  return true;
}

}  // namespace micro_wake_word
}  // namespace esphome

#endif  // USE_ESP_IDF

#endif  // CLANG_TIDY
