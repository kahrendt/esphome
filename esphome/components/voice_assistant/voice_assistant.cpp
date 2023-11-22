#include "voice_assistant.h"

#ifdef USE_VOICE_ASSISTANT

#include "esphome/core/log.h"

#include <cstdio>

// #include "audio_provider.h"
// // #include "command_responder.h"
// #include "feature_provider.h"
#include "micro_model_settings.h"
#include "model.h"
// #include "recognize_commands.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "micro_features_generator.h"
#include "micro_model_settings.h"

namespace esphome {
namespace voice_assistant {

Features g_features;
static const char *const TAG = "voice_assistant";

#ifdef SAMPLE_RATE_HZ
#undef SAMPLE_RATE_HZ
#endif

static const size_t SAMPLE_RATE_HZ = 16000;
static const size_t INPUT_BUFFER_SIZE = 32 * SAMPLE_RATE_HZ / 1000;  // 32ms * 16kHz / 1000ms
static const size_t BUFFER_SIZE = 1000 * SAMPLE_RATE_HZ / 1000;      // 1s
static const size_t SEND_BUFFER_SIZE = INPUT_BUFFER_SIZE * sizeof(int16_t);
static const size_t RECEIVE_SIZE = 1024;
static const size_t SPEAKER_BUFFER_SIZE = 16 * RECEIVE_SIZE;

float VoiceAssistant::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void VoiceAssistant::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Assistant...");

  global_voice_assistant = this;

  this->socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (socket_ == nullptr) {
    ESP_LOGW(TAG, "Could not create socket");
    this->mark_failed();
    return;
  }
  int enable = 1;
  int err = socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  if (err != 0) {
    ESP_LOGW(TAG, "Socket unable to set reuseaddr: errno %d", err);
    // we can still continue
  }
  err = socket_->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Socket unable to set nonblocking mode: errno %d", err);
    this->mark_failed();
    return;
  }

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    struct sockaddr_storage server;

    socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &server, sizeof(server), 6055);
    if (sl == 0) {
      ESP_LOGW(TAG, "Socket unable to set sockaddr: errno %d", errno);
      this->mark_failed();
      return;
    }

    err = socket_->bind((struct sockaddr *) &server, sizeof(server));
    if (err != 0) {
      ESP_LOGW(TAG, "Socket unable to bind: errno %d", errno);
      this->mark_failed();
      return;
    }

    ExternalRAMAllocator<uint8_t> speaker_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->speaker_buffer_ = speaker_allocator.allocate(SPEAKER_BUFFER_SIZE);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGW(TAG, "Could not allocate speaker buffer");
      this->mark_failed();
      return;
    }
  }
#endif

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->input_buffer_ = allocator.allocate(INPUT_BUFFER_SIZE);
  if (this->input_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate input buffer");
    this->mark_failed();
    return;
  }

#ifdef USE_ESP_ADF
  this->vad_instance_ = vad_create(VAD_MODE_4);

  this->ring_buffer_ = rb_create(BUFFER_SIZE, sizeof(int16_t));
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }
#endif

  ExternalRAMAllocator<uint8_t> send_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->send_buffer_ = send_allocator.allocate(SEND_BUFFER_SIZE);
  if (send_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer");
    this->mark_failed();
    return;
  }

  ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->tensor_arena_ = arena_allocator.allocate(this->kTensorArenaSize_);
  if (this->tensor_arena_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }

  this->var_arena_ = arena_allocator.allocate(64000);
  if (this->var_arena_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }

  ExternalRAMAllocator<float> feature_buffer_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  this->feature_buffer_ = feature_buffer_allocator.allocate(kFeatureElementCount);
  if (this->feature_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }

  ExternalRAMAllocator<float> features_output_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  this->features_output_ = features_output_allocator.allocate(kFeatureElementCount);
  if (this->features_output_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }

  ExternalRAMAllocator<int16_t> audio_output_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->g_audio_output_buffer_ = audio_output_allocator.allocate(kMaxAudioSampleSize * 32);
  if (this->g_audio_output_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }
  ExternalRAMAllocator<int16_t> history_buffer_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->g_history_buffer_ = history_buffer_allocator.allocate(history_samples_to_keep);
  if (this->g_history_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer.");
    this->mark_failed();
    return;
  }

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(g_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model provided is schema version %d not equal to supported "
                "version %d.",
                model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver micro_op_resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroMutableOpResolver<12> micro_op_resolver;
  // if (micro_op_resolver.AddCallOnce() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddVarHandle() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddReadVariable() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddFullyConnected() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddSplitV() != kTfLiteOk) {
    return;
  }

  if (micro_op_resolver.AddStridedSlice() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddConcatenation() != kTfLiteOk) {
    return;
  }
  // if (micro_op_resolver.AddAssignVariable() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddReshape() != kTfLiteOk) {
    return;
  }

  // if (micro_op_resolver.AddMaxPool2D() != kTfLiteOk) {
  //   return;
  // }

  // if (micro_op_resolver.AddTranspose() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddBatchMatMul() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddSum() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddTanh() != kTfLiteOk) {
    return;
  }
  // if (micro_op_resolver.AddFullyConnected() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddSoftmax() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddConv2D() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddSplit() != kTfLiteOk) {
    return;
  }
  // if (micro_op_resolver.AddSoftmax() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddReshape() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddStridedSlice() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddExpandDims() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddTranspose() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddBatchMatMul() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddSum() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddSub() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddAdd() != kTfLiteOk) {
    return;
  }
  // if (micro_op_resolver.AddMaxPool2D() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddDequantize() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddWhile() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddQuantize() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddLess() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddLogicalAnd() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddMul() != kTfLiteOk) {
    return;
  }
  // if (micro_op_resolver.AddGather() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddMean() != kTfLiteOk) {
  //   return;
  // }
  // if (micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk) {
  //   return;
  // }
  if (micro_op_resolver.AddLogistic() != kTfLiteOk) {
    return;
  }
  // static uint8_t var_arena[52000];
  static tflite::MicroAllocator *ma = tflite::MicroAllocator::Create(this->var_arena_, 64000);
  static tflite::MicroResourceVariables *mrv = tflite::MicroResourceVariables::Create(ma, 7);

  // Build an interpreter to run the model with.
  static tflite::MicroInterpreter static_interpreter(model, micro_op_resolver, this->tensor_arena_, kTensorArenaSize_,
                                                     mrv);
  // static tflite::MicroInterpreter static_interpreter(model, micro_op_resolver, this->tensor_arena_,
  // kTensorArenaSize_);
  interpreter_kws = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter_kws->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    ESP_LOGE(TAG, "AllocateTensors() failed");
    this->mark_failed();
    return;
  } else {
    ESP_LOGE(TAG, "AllocateTEnsors() succeeded");
  }

  // Get information about the memory area to use for the model's input.
  model_input = interpreter_kws->input(0);
  ESP_LOGE(TAG, "model_input_size=%u, dimension1=%u, dimension2=%u", model_input->dims->size,
           model_input->dims->data[0], model_input->dims->data[1]);
  // if ((model_input->dims->size != 2) || (model_input->dims->data[0] != 1) ||
  //     (model_input->dims->data[1] != (kFeatureCount * kFeatureSize)) || (model_input->type != kTfLiteInt8)) {
  //   ESP_LOGE(TAG, "Bad input tensor parameters in model");
  //   this->mark_failed();
  //   return;
  // }
  // model_input_buffer = tflite::GetTensorData<float>(model_input);

  // memory_input = interpreter_kws->input(1);
  // memory_output = interpreter_kws->output(1);

  // ESP_LOGD(TAG,
  //          "output_memory_tensor_size=%u, output_memory_tensor_d1=%u, output_memory_tensor_d2=%u, "
  //          "output_memory_tensor_d3=%u",
  //          memory_output->dims->size, memory_output->dims->data[0], memory_output->dims->data[1],
  //          memory_output->dims->data[2]);

  // Prepare to access the audio spectrograms from a microphone or other source
  // that will provide the inputs to the neural network.
  // NOLINTNEXTLINE(runtime-global-variables)2
  // static FeatureProvider static_feature_provider(kFeatureElementCount, feature_buffer);
  // feature_provider = &static_feature_provider;
  for (int n = 0; n < feature_size_; ++n) {
    this->feature_buffer_[n] = 0.0;
  }

  for (int i = 0; i < 3 * 40; ++i) {
    interpreter_kws->output(1)->data.f[i] = 0.0;
  }
  for (int i = 0; i < 5 * 38 * 16; ++i) {
    interpreter_kws->output(2)->data.f[i] = 0.0;
  }
  for (int i = 0; i < 1 * 256; ++i) {
    interpreter_kws->output(3)->data.f[i] = 0.0;
  }
  for (int i = 0; i < 1 * 256; ++i) {
    interpreter_kws->output(4)->data.f[i] = 0.0;
  }
  // for (int i = 0; i < 1 * 57 * 1 * 64; ++i) {
  //   interpreter_kws->output(5)->data.f[i] = 0.0;
  // }
  // for (int i = 0; i < 1 * 1 * 1 * 128; ++i) {
  //   interpreter_kws->output(6)->data.f[i] = 0.0;
  // }
  // for (int i = 0; i < 1 * 49 * 1 * 128; ++i) {
  //   interpreter_kws->output(7)->data.f[i] = 0.0;
  // }

  // for (int n = 0; n < 1 * 49 * 128; ++n) {
  //   this->memory_input->data.f[n] = 0.0;
  //   this->memory_output->data.f[n] = 0.0;
  // }

  previous_time = 0;

#ifdef TF_LITE_STATIC_MEMORY
  ESP_LOGCONFIG(TAG, "Using TF_LITE_STATIC_MEMORY\n");
#endif

// Output whether we are using esp nn
#ifdef ESP_NN
  ESP_LOGCONFIG(TAG, "Using ESP_NN\n");
#endif

// Output whether the neural network is optimised
#ifdef CONFIG_NN_OPTIMIZED
  ESP_LOGCONFIG(TAG, "Using CONFIG_NN_OPTIMIZED\n");
#endif
}

int VoiceAssistant::read_microphone_() {
  size_t bytes_read = 0;
  if (this->mic_->is_running()) {  // Read audio into input buffer
    // bytes_read = this->mic_->read(this->input_buffer_, 3200);
    bytes_read = this->mic_->read(this->input_buffer_, INPUT_BUFFER_SIZE * sizeof(int16_t));
    if (bytes_read == 0) {
      memset(this->input_buffer_, 0, INPUT_BUFFER_SIZE * sizeof(int16_t));
      return 0;
    }
#ifdef USE_ESP_ADF
    // Write audio into ring buffer
    int available = rb_bytes_available(this->ring_buffer_);
    if (available < bytes_read) {
      rb_read(this->ring_buffer_, nullptr, bytes_read - available, 0);
    }
    rb_write(this->ring_buffer_, (char *) this->input_buffer_, bytes_read, 0);
    g_latest_audio_timestamp = g_latest_audio_timestamp + ((1000 * (bytes_read / 2)) / kAudioSampleFrequency);
#endif
  } else {
    ESP_LOGD(TAG, "microphone not running");
  }

  return bytes_read;
}

void VoiceAssistant::loop() {
  if (this->api_client_ == nullptr && this->state_ != State::IDLE && this->state_ != State::STOP_MICROPHONE &&
      this->state_ != State::STOPPING_MICROPHONE) {
    if (this->mic_->is_running() || this->state_ == State::STARTING_MICROPHONE) {
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
    } else {
      this->set_state_(State::IDLE, State::IDLE);
    }
    this->continuous_ = false;
    this->signal_stop_();
    return;
  }
  switch (this->state_) {
    case State::IDLE: {
      if (this->continuous_ && this->desired_state_ == State::IDLE) {
#ifdef USE_ESP_ADF
        if (this->use_wake_word_) {
          rb_reset(this->ring_buffer_);
          this->set_state_(State::START_MICROPHONE, State::WAIT_FOR_VAD);
        } else if (this->use_local_wake_word_) {
          rb_reset(this->ring_buffer_);
          this->set_state_(State::START_MICROPHONE, State::WAIT_FOR_WAKE_WORD);
        } else
#endif
        {
          { this->set_state_(State::START_PIPELINE, State::START_MICROPHONE); }
        }
      } else {
        this->high_freq_.stop();
      }
      break;
    }
    case State::START_MICROPHONE: {
      ESP_LOGD(TAG, "Starting Microphone");
      memset(this->send_buffer_, 0, SEND_BUFFER_SIZE);
      memset(this->input_buffer_, 0, INPUT_BUFFER_SIZE * sizeof(int16_t));
      this->mic_->start();
      this->high_freq_.start();
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    }
    case State::STARTING_MICROPHONE: {
      if (this->mic_->is_running()) {
        this->set_state_(this->desired_state_);
      }
      break;
    }
#ifdef USE_ESP_ADF
    case State::WAIT_FOR_WAKE_WORD: {
      this->read_microphone_();
      ESP_LOGD(TAG, "Waiting for wake word...");
      this->set_state_(State::WAITING_FOR_WAKE_WORD);
      break;
    }
    case State::WAITING_FOR_WAKE_WORD: {
      size_t bytes_read = this->read_microphone_();

      const int32_t current_time = LatestAudioTimestamp();
      int how_many_new_slices = 0;

      TfLiteStatus feature_status = this->PopulateFeatureData(previous_time, current_time, &how_many_new_slices);
      previous_time = current_time;

      if (feature_status != kTfLiteOk) {
        ESP_LOGD(TAG, "Feature generation failed");
        return;
      }

      // if (millis() - this->last_wake_word_check_ < 200) {
      //   return;
      // }
      // this->last_wake_word_check_ = millis();

      // float max_result = 0.0;
      // int max_idx = 0;
      // for (int i = 0; i < kCategoryCount; i++) {
      //   float current_result = output_probabilities_[i];
      //   if (current_result > max_result) {
      //     max_result = current_result;  // update max result
      //     max_idx = i;                  // update category
      //   }
      // }

      // // ESP_LOGD(TAG, "silence=%.3f,unknown=%.3f,computer=%.3f", output->data.f[0], output->data.f[1],
      // // output->data.f[2]);

      // // // if (max_result > 0.8f) {
      // ESP_LOGD(TAG, "Detected %7s, score: %.5f", kCategoryLabels[max_idx], static_cast<double>(max_result));

      // if (max_idx == 2) {
      //   this->set_state_(State::START_PIPELINE, State::STREAMING_MICROPHONE);
      // }
      // }

      // // Copy feature buffer to input tensor
      // for (int i = 0; i < kFeatureSize; i++) {
      //   model_input_buffer[i] = this->feature_buffer_[i];
      // }

      // // Run the model on the spectrogram input and make sure it succeeds.
      // TfLiteStatus invoke_status = interpreter->Invoke();
      // if (invoke_status != kTfLiteOk) {
      //   ESP_LOGD(TAG, "Invoke failed");
      //   return;
      // }

      // // Obtain a pointer to the output tensor
      // TfLiteTensor *output = interpreter->output(0);

      // float output_scale = output->params.scale;
      // int output_zero_point = output->params.zero_point;
      // int max_idx = 0;
      // float max_result = 0.0;
      // // Dequantize output values and find the max
      // for (int i = 0; i < kCategoryCount; i++) {
      //   float current_result = (tflite::GetTensorData<int8_t>(output)[i] - output_zero_point) * output_scale;
      //   if (current_result > max_result) {
      //     max_result = current_result;  // update max result
      //     max_idx = i;                  // update category
      //   }
      // }
      // if (max_result > 0.8f) {
      //   ESP_LOGD(TAG, "Detected %7s, score: %.2f", kCategoryLabels[max_idx], static_cast<double>(max_result));
      //   // if (max_idx == 2) {
      //   //   this->set_state_(State::START_PIPELINE, State::STREAMING_MICROPHONE);
      //   // }
      // }

      // if ((max_result > 0.95f) && (max_idx == 2)) {
      //   ++this->succesive_wake_words;
      // } else {
      //   if (this->succesive_wake_words > 0) {
      //     --this->succesive_wake_words;
      //   }
      // }
      // // if ((max_result > 0.95f) && (max_idx == 2)) {
      // //   if (this->last_probability > 0.95f) {
      // //     ++this->succesive_wake_words;
      // //   }
      // //   this->last_probability = max_result;
      // // } else {
      // //   this->last_probability = 0.0;
      // //   this->succesive_wake_words = 0;
      // // }

      // if (this->succesive_wake_words >= 5) {
      //   ESP_LOGD(TAG, "Wakeword detected");
      //   this->succesive_wake_words = 0;
      //   // this->last_probability = 0.0f;
      //   this->set_state_(State::START_PIPELINE, State::STREAMING_MICROPHONE);
      // }
      break;
    }
    case State::WAIT_FOR_VAD: {
      this->read_microphone_();
      ESP_LOGD(TAG, "Waiting for speech...");
      this->set_state_(State::WAITING_FOR_VAD);
      break;
    }
    case State::WAITING_FOR_VAD: {
      size_t bytes_read = this->read_microphone_();

      if (bytes_read > 0) {
        vad_state_t vad_state =
            vad_process(this->vad_instance_, this->input_buffer_, SAMPLE_RATE_HZ, VAD_FRAME_LENGTH_MS);
        if (vad_state == VAD_SPEECH) {
          if (this->vad_counter_ < this->vad_threshold_) {
            this->vad_counter_++;
          } else {
            ESP_LOGD(TAG, "VAD detected speech");
            this->set_state_(State::START_PIPELINE, State::STREAMING_MICROPHONE);

            // Reset for next time
            this->vad_counter_ = 0;
          }
        } else {
          if (this->vad_counter_ > 0) {
            this->vad_counter_--;
          }
        }
      }
      break;
    }
#endif
    case State::START_PIPELINE: {
      this->read_microphone_();
      ESP_LOGD(TAG, "Requesting start...");
      uint32_t flags = 0;
      if (this->use_wake_word_)
        flags |= api::enums::VOICE_ASSISTANT_REQUEST_USE_WAKE_WORD;
      if (this->silence_detection_)
        flags |= api::enums::VOICE_ASSISTANT_REQUEST_USE_VAD;
      api::VoiceAssistantAudioSettings audio_settings;
      audio_settings.noise_suppression_level = this->noise_suppression_level_;
      audio_settings.auto_gain = this->auto_gain_;
      audio_settings.volume_multiplier = this->volume_multiplier_;

      api::VoiceAssistantRequest msg;
      msg.start = true;
      msg.conversation_id = this->conversation_id_;
      msg.flags = flags;
      msg.audio_settings = audio_settings;

      if (this->api_client_ == nullptr || !this->api_client_->send_voice_assistant_request(msg)) {
        ESP_LOGW(TAG, "Could not request start");
        this->error_trigger_->trigger("not-connected", "Could not request start");
        this->continuous_ = false;
        this->set_state_(State::IDLE, State::IDLE);
        break;
      }
      this->set_state_(State::STARTING_PIPELINE);
      this->set_timeout("reset-conversation_id", 5 * 60 * 1000, [this]() { this->conversation_id_ = ""; });
      break;
    }
    case State::STARTING_PIPELINE: {
      this->read_microphone_();
      break;  // State changed when udp server port received
    }
    case State::STREAMING_MICROPHONE: {
      size_t bytes_read = this->read_microphone_();
#ifdef USE_ESP_ADF
      if (rb_bytes_filled(this->ring_buffer_) >= SEND_BUFFER_SIZE) {
        rb_read(this->ring_buffer_, (char *) this->send_buffer_, SEND_BUFFER_SIZE, 0);
        this->socket_->sendto(this->send_buffer_, SEND_BUFFER_SIZE, 0, (struct sockaddr *) &this->dest_addr_,
                              sizeof(this->dest_addr_));
      }
#else
      if (bytes_read > 0) {
        this->socket_->sendto(this->input_buffer_, bytes_read, 0, (struct sockaddr *) &this->dest_addr_,
                              sizeof(this->dest_addr_));
      }
#endif
      break;
    }
    case State::STOP_MICROPHONE: {
      if (this->mic_->is_running()) {
        this->mic_->stop();
        this->set_state_(State::STOPPING_MICROPHONE);
      } else {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    case State::STOPPING_MICROPHONE: {
      if (this->mic_->is_stopped()) {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    case State::AWAITING_RESPONSE: {
      break;  // State changed by events
    }
    case State::STREAMING_RESPONSE: {
      bool playing = false;
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        ssize_t received_len = 0;
        if (this->speaker_buffer_index_ + RECEIVE_SIZE < SPEAKER_BUFFER_SIZE) {
          received_len = this->socket_->read(this->speaker_buffer_ + this->speaker_buffer_index_, RECEIVE_SIZE);
          if (received_len > 0) {
            this->speaker_buffer_index_ += received_len;
            this->speaker_buffer_size_ += received_len;
            this->speaker_bytes_received_ += received_len;
          }
        } else {
          ESP_LOGD(TAG, "Receive buffer full");
        }
        // Build a small buffer of audio before sending to the speaker
        if (this->speaker_bytes_received_ > RECEIVE_SIZE * 4)
          this->write_speaker_();
        if (this->wait_for_stream_end_) {
          this->cancel_timeout("playing");
          if (this->stream_ended_ && received_len < 0) {
            ESP_LOGD(TAG, "End of audio stream received");
            this->cancel_timeout("speaker-timeout");
            this->set_state_(State::RESPONSE_FINISHED, State::RESPONSE_FINISHED);
          }
          break;  // We dont want to timeout here as the STREAM_END event will take care of that.
        }
        playing = this->speaker_->is_running();
      }
#endif
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        playing = (this->media_player_->state == media_player::MediaPlayerState::MEDIA_PLAYER_STATE_PLAYING);
      }
#endif
      if (playing) {
        this->set_timeout("playing", 2000, [this]() {
          this->cancel_timeout("speaker-timeout");
          this->set_state_(State::IDLE, State::IDLE);
        });
      }
      break;
    }
    case State::RESPONSE_FINISHED: {
#ifdef USE_SPEAKER
      if (this->speaker_ != nullptr) {
        if (this->speaker_buffer_size_ > 0) {
          this->write_speaker_();
          break;
        }
        if (this->speaker_->has_buffered_data() || this->speaker_->is_running()) {
          break;
        }
        ESP_LOGD(TAG, "Speaker has finished outputting all audio");
        this->speaker_->stop();
        this->cancel_timeout("speaker-timeout");
        this->cancel_timeout("playing");
        this->speaker_buffer_size_ = 0;
        this->speaker_buffer_index_ = 0;
        this->speaker_bytes_received_ = 0;
        memset(this->speaker_buffer_, 0, SPEAKER_BUFFER_SIZE);
        this->wait_for_stream_end_ = false;
        this->stream_ended_ = false;

        this->tts_stream_end_trigger_->trigger();
      }
#endif
      this->set_state_(State::IDLE, State::IDLE);
      break;
    }
    default:
      break;
  }
}

TfLiteStatus VoiceAssistant::PopulateFeatureData(int32_t last_time_in_ms, int32_t time_in_ms,
                                                 int *how_many_new_slices) {
  if (feature_size_ != kFeatureElementCount) {
    MicroPrintf("Requested feature_buffer size %d doesn't match %d", feature_size_, kFeatureElementCount);
    return kTfLiteError;
  }

  // Quantize the time into steps as long as each window stride, so we can
  // figure out which audio data we need to fetch.
  const int last_step = (last_time_in_ms / kFeatureStrideMs);
  const int current_step = (time_in_ms / kFeatureStrideMs);

  int slices_needed = current_step - last_step;
  // If this is the first call, make sure we don't use any cached information.
  if (is_first_run_) {
    TfLiteStatus init_status = InitializeMicroFeatures();
    if (init_status != kTfLiteOk) {
      return init_status;
    }
    ESP_LOGI(TAG, "InitializeMicroFeatures successful");
    is_first_run_ = false;
    slices_needed = 1;
  }
  *how_many_new_slices = slices_needed;
  if (slices_needed > 0) {
    for (int new_slice = 0; new_slice < slices_needed; ++new_slice) {
      // const int new_step = (current_step - kFeatureCount + 1) + new_slice;
      // const int32_t slice_start_ms = (new_step * kFeatureStrideMs);
      int16_t *audio_samples = nullptr;
      int audio_samples_size = 0;
      // TODO(petewarden): Fix bug that leads to non-zero slice_start_ms
      if (GetAudioSamples(&audio_samples_size, &audio_samples) != kTfLiteOk) {
        return kTfLiteError;
      }

      TfLiteStatus generate_status =
          GenerateSingleFloatFeature(audio_samples, audio_samples_size, this->features_output_);

      // std::memcpy(tflite::GetTensorData<float>(this->interpreter_kws->input(0)), this->features_output_,
      // kFeatureSize * sizeof(float));

      for (int i = 0; i < kFeatureSize; ++i) {
        interpreter_kws->input(0)->data.f[i] = this->features_output_[i];
      }

      // TfLiteTensor *input = interpreter_kws->input(0);
      TfLiteTensor *input_mem_1 = interpreter_kws->input(1);
      TfLiteTensor *input_mem_2 = interpreter_kws->input(2);
      TfLiteTensor *input_mem_3 = interpreter_kws->input(3);
      TfLiteTensor *input_mem_4 = interpreter_kws->input(4);
      // TfLiteTensor *input_mem_5 = interpreter_kws->input(5);
      // TfLiteTensor *input_mem_6 = interpreter_kws->input(6);
      // TfLiteTensor *input_mem_7 = interpreter_kws->input(7);

      TfLiteTensor *output_mem_1 = interpreter_kws->output(1);
      TfLiteTensor *output_mem_2 = interpreter_kws->output(2);
      TfLiteTensor *output_mem_3 = interpreter_kws->output(3);
      TfLiteTensor *output_mem_4 = interpreter_kws->output(4);
      // TfLiteTensor *output_mem_5 = interpreter_kws->output(5);
      // TfLiteTensor *output_mem_6 = interpreter_kws->output(6);
      // TfLiteTensor *output_mem_7 = interpreter_kws->output(7);

      std::memcpy(tflite::GetTensorData<float>(input_mem_1), tflite::GetTensorData<float>(output_mem_1),
                  3 * 40 * sizeof(float));
      std::memcpy(tflite::GetTensorData<float>(input_mem_2), tflite::GetTensorData<float>(output_mem_2),
                  5 * 38 * 16 * sizeof(float));
      std::memcpy(tflite::GetTensorData<float>(input_mem_3), tflite::GetTensorData<float>(output_mem_3),
                  256 * sizeof(float));
      std::memcpy(tflite::GetTensorData<float>(input_mem_4), tflite::GetTensorData<float>(output_mem_4),
                  256 * sizeof(float));

      // std::memcpy(tflite::GetTensorData<float>(input_mem_1), tflite::GetTensorData<float>(output_mem_1),
      //             input_mem_1->dims->data[1] * input_mem_1->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_2), tflite::GetTensorData<float>(output_mem_2),
      //             input_mem_2->dims->data[1] * input_mem_2->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_3), tflite::GetTensorData<float>(output_mem_3),
      //             input_mem_3->dims->data[1] * input_mem_3->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_4), tflite::GetTensorData<float>(output_mem_4),
      //             input_mem_4->dims->data[1] * input_mem_4->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_5), tflite::GetTensorData<float>(output_mem_5),
      //             input_mem_5->dims->data[1] * input_mem_5->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_6), tflite::GetTensorData<float>(output_mem_6),
      //             input_mem_6->dims->data[1] * input_mem_6->dims->data[3] * sizeof(float));
      // std::memcpy(tflite::GetTensorData<float>(input_mem_7), tflite::GetTensorData<float>(output_mem_7),
      //             input_mem_7->dims->data[1] * input_mem_7->dims->data[3] * sizeof(float));

      // for (int i = 0; i < 1 * 11 * 1 * 40; ++i) {
      //   interpreter_kws->input(1)->data.f[i] = interpreter_kws->output(1)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 13 * 1 * 128; ++i) {
      //   interpreter_kws->input(2)->data.f[i] = interpreter_kws->output(2)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 15 * 1 * 64; ++i) {
      //   interpreter_kws->input(3)->data.f[i] = interpreter_kws->output(3)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 17 * 1 * 64; ++i) {
      //   interpreter_kws->input(4)->data.f[i] = interpreter_kws->output(4)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 57 * 1 * 64; ++i) {
      //   interpreter_kws->input(5)->data.f[i] = interpreter_kws->output(5)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 1 * 1 * 128; ++i) {
      //   interpreter_kws->input(6)->data.f[i] = interpreter_kws->output(6)->data.f[i];
      // }
      // for (int i = 0; i < 1 * 49 * 1 * 128; ++i) {
      //   interpreter_kws->input(7)->data.f[i] = interpreter_kws->output(7)->data.f[i];
      // }

      // Run the model on the spectrogram input and make sure it succeeds.
      uint32_t current_time = millis();
      TfLiteStatus invoke_status = interpreter_kws->Invoke();
      ESP_LOGD(TAG, "Inference time = %ums", (millis() - current_time));
      // if (invoke_status != kTfLiteOk) {
      //   ESP_LOGD(TAG, "Invoke failed");
      //   return kTfLiteError;
      // }

      // // Obtain a pointer to the output tensor
      // TfLiteTensor *output = interpreter_kws->output(0);

      // for (int i = 0; i < 3; ++i) {
      //   this->output_probabilities_[i] = output->data.f[i];
      // }

      // return generate_status;
    }
  }

  // #if 1
  //   if (slices_needed > kFeatureCount) {
  //     slices_needed = kFeatureCount;
  //   }
  //   *how_many_new_slices = slices_needed;

  //   const int slices_to_keep = kFeatureCount - slices_needed;
  //   const int slices_to_drop = kFeatureCount - slices_to_keep;
  //   // If we can avoid recalculating some slices, just move the existing data
  //   // up in the spectrogram, to perform something like this:
  //   // last time = 80ms          current time = 120ms
  //   // +-----------+             +-----------+
  //   // | data@20ms |         --> | data@60ms |
  //   // +-----------+       --    +-----------+
  //   // | data@40ms |     --  --> | data@80ms |
  //   // +-----------+   --  --    +-----------+
  //   // | data@60ms | --  --      |  <empty>  |
  //   // +-----------+   --        +-----------+
  //   // | data@80ms | --          |  <empty>  |
  //   // +-----------+             +-----------+
  //   if (slices_to_keep > 0) {
  //     for (int dest_slice = 0; dest_slice < slices_to_keep; ++dest_slice) {
  //       float *dest_slice_data = this->feature_buffer_ + (dest_slice * kFeatureSize);
  //       const int src_slice = dest_slice + slices_to_drop;
  //       const float *src_slice_data = this->feature_buffer_ + (src_slice * kFeatureSize);
  //       for (int i = 0; i < kFeatureSize; ++i) {
  //         dest_slice_data[i] = src_slice_data[i];
  //       }
  //     }
  //   }
  //   // Any slices that need to be filled in with feature data have their
  //   // appropriate audio data pulled, and features calculated for that slice.
  //   if (slices_needed > 0) {
  //     for (int new_slice = slices_to_keep; new_slice < kFeatureCount; ++new_slice) {
  //       const int new_step = (current_step - kFeatureCount + 1) + new_slice;
  //       const int32_t slice_start_ms = (new_step * kFeatureStrideMs);
  //       int16_t *audio_samples = nullptr;
  //       int audio_samples_size = 0;
  //       // TODO(petewarden): Fix bug that leads to non-zero slice_start_ms
  //       if (GetAudioSamples((slice_start_ms > 0 ? slice_start_ms : 0), kFeatureDurationMs, &audio_samples_size,
  //                           &audio_samples) != kTfLiteOk) {
  //         return kTfLiteError;
  //       }
  //       if (audio_samples_size < kMaxAudioSampleSize) {
  //         ESP_LOGD(TAG, "Audio data size %d too small, want %d", audio_samples_size, kMaxAudioSampleSize);
  //         return kTfLiteError;
  //       }
  //       float *new_slice_data = this->feature_buffer_ + (new_slice * kFeatureSize);
  //       // size_t num_samples_read;
  //       // TfLiteStatus generate_status = GenerateMicroFeatures(
  //       //     audio_samples, audio_samples_size, kFeatureSize,
  //       //     new_slice_data, &num_samples_read);
  //       TfLiteStatus generate_status = GenerateFeatures(audio_samples, audio_samples_size, &g_features);
  //       if (generate_status != kTfLiteOk) {
  //         return generate_status;
  //       }

  //       // copy features
  //       for (int j = 0; j < kFeatureSize; ++j) {
  //         new_slice_data[j] = g_features[0][j];
  //       }
  //     }
  //   }
  // #else
  // *how_many_new_slices = kFeatureCount;
  // int16_t *audio_samples = nullptr;
  // int audio_samples_size = 16000;
  // if (GetAudioSamples(0, kFeatureDurationMs, &audio_samples_size, &audio_samples) != kTfLiteOk) {
  //   return kTfLiteError;
  // }

  // memset(g_features, 0, sizeof(g_features));

  // TfLiteStatus generate_status = GenerateFeatures(audio_samples, audio_samples_size, &g_features);
  // if (generate_status != kTfLiteOk) {
  //   return generate_status;
  // }
  // // copy features
  // for (int i = 0; i < kFeatureCount; ++i) {
  //   for (int j = 0; j < kFeatureSize; ++j) {
  //     this->feature_buffer_[i * kFeatureSize + j] = g_features[i][j];
  //   }
  // }
  // vTaskDelay(pdMS_TO_TICKS(500));
  // #endif
  return kTfLiteOk;
}

// TfLiteStatus VoiceAssistant::GetAudioSamples1(int *audio_samples_size, int16_t **audio_samples) {
//   int bytes_read = rb_read(this->ring_buffer_, (char *) (g_audio_output_buffer), 16000, 1000);
//   if (bytes_read < 0) {
//     ESP_LOGI(TAG, "Couldn't read data in time");
//     bytes_read = 0;
//   }
//   *audio_samples_size = bytes_read;
//   *audio_samples = g_audio_output_buffer;
//   return kTfLiteOk;
// }

TfLiteStatus VoiceAssistant::GetAudioSamples(int *audio_samples_size, int16_t **audio_samples) {
  // if (!g_is_audio_initialized) {
  //   TfLiteStatus init_status = InitAudioRecording();
  //   if (init_status != kTfLiteOk) {
  //     return init_status;
  //   }
  //   g_is_audio_initialized = true;
  // }
  /* copy 160 samples (320 bytes) into output_buff from history */
  memcpy((void *) (this->g_audio_output_buffer_), (void *) (this->g_history_buffer_),
         history_samples_to_keep * sizeof(int16_t));

  /* copy 320 samples (640 bytes) from rb at ( int16_t*(g_audio_output_buffer) +
   * 160 ), first 160 samples (320 bytes) will be from history */
  if (rb_bytes_filled(this->ring_buffer_) < new_samples_to_get * sizeof(int16_t)) {
    ESP_LOGD(TAG, " Buffer not full enough");
    return kTfLiteError;
  }
  int bytes_read = rb_read(this->ring_buffer_, ((char *) (this->g_audio_output_buffer_ + history_samples_to_keep)),
                           new_samples_to_get * sizeof(int16_t), pdMS_TO_TICKS(200));
  if (bytes_read < 0) {
    ESP_LOGE(TAG, " Model Could not read data from Ring Buffer");
  } else if (bytes_read < new_samples_to_get * sizeof(int16_t)) {
    // ESP_LOGD(TAG, "RB FILLED RIGHT NOW IS %d", rb_filled(ring_buffer));
    ESP_LOGD(TAG, " Partial Read of Data by Model ");
    ESP_LOGD(TAG, " Could only read %d bytes when required %d bytes ", bytes_read,
             (int) (new_samples_to_get * sizeof(int16_t)));
    return kTfLiteError;
  }

  /* copy 320 bytes from output_buff into history */
  memcpy((void *) (this->g_history_buffer_), (void *) (this->g_audio_output_buffer_ + new_samples_to_get),
         history_samples_to_keep * sizeof(int16_t));

  // *audio_samples_size = kMaxAudioSampleSize;
  // *audio_samples_size = bytes_read / sizeof(int16_t);
  *audio_samples = this->g_audio_output_buffer_;
  return kTfLiteOk;
}

int32_t VoiceAssistant::LatestAudioTimestamp() { return g_latest_audio_timestamp; }

#ifdef USE_SPEAKER
void VoiceAssistant::write_speaker_() {
  if (this->speaker_buffer_size_ > 0) {
    size_t written = this->speaker_->play(this->speaker_buffer_, this->speaker_buffer_size_);
    if (written > 0) {
      memmove(this->speaker_buffer_, this->speaker_buffer_ + written, this->speaker_buffer_size_ - written);
      this->speaker_buffer_size_ -= written;
      this->speaker_buffer_index_ -= written;
      this->set_timeout("speaker-timeout", 5000, [this]() { this->speaker_->stop(); });
    } else {
      ESP_LOGD(TAG, "Speaker buffer full, trying again next loop");
    }
  }
}
#endif

void VoiceAssistant::client_subscription(api::APIConnection *client, bool subscribe) {
  if (!subscribe) {
    if (this->api_client_ == nullptr || client != this->api_client_) {
      ESP_LOGE(TAG, "Client attempting to unsubscribe that is not the current API Client");
      return;
    }
    this->api_client_ = nullptr;
    this->client_disconnected_trigger_->trigger();
    return;
  }

  if (this->api_client_ != nullptr) {
    ESP_LOGE(TAG, "Multiple API Clients attempting to connect to Voice Assistant");
    ESP_LOGE(TAG, "Current client: %s", this->api_client_->get_client_combined_info().c_str());
    ESP_LOGE(TAG, "New client: %s", client->get_client_combined_info().c_str());
    return;
  }

  this->api_client_ = client;
  this->client_connected_trigger_->trigger();
}

static const LogString *voice_assistant_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return LOG_STR("IDLE");
    case State::START_MICROPHONE:
      return LOG_STR("START_MICROPHONE");
    case State::STARTING_MICROPHONE:
      return LOG_STR("STARTING_MICROPHONE");
    case State::WAIT_FOR_WAKE_WORD:
      return LOG_STR("WAIT_FOR_WAKE_WORD");
    case State::WAITING_FOR_WAKE_WORD:
      return LOG_STR("WAITING_FOR_WAKE_WORD");
    case State::WAIT_FOR_VAD:
      return LOG_STR("WAIT_FOR_VAD");
    case State::WAITING_FOR_VAD:
      return LOG_STR("WAITING_FOR_VAD");
    case State::START_PIPELINE:
      return LOG_STR("START_PIPELINE");
    case State::STARTING_PIPELINE:
      return LOG_STR("STARTING_PIPELINE");
    case State::STREAMING_MICROPHONE:
      return LOG_STR("STREAMING_MICROPHONE");
    case State::STOP_MICROPHONE:
      return LOG_STR("STOP_MICROPHONE");
    case State::STOPPING_MICROPHONE:
      return LOG_STR("STOPPING_MICROPHONE");
    case State::AWAITING_RESPONSE:
      return LOG_STR("AWAITING_RESPONSE");
    case State::STREAMING_RESPONSE:
      return LOG_STR("STREAMING_RESPONSE");
    case State::RESPONSE_FINISHED:
      return LOG_STR("RESPONSE_FINISHED");
    default:
      return LOG_STR("UNKNOWN");
  }
};

void VoiceAssistant::set_state_(State state) {
  State old_state = this->state_;
  this->state_ = state;
  ESP_LOGD(TAG, "State changed from %s to %s", LOG_STR_ARG(voice_assistant_state_to_string(old_state)),
           LOG_STR_ARG(voice_assistant_state_to_string(state)));
}

void VoiceAssistant::set_state_(State state, State desired_state) {
  this->set_state_(state);
  this->desired_state_ = desired_state;
  ESP_LOGD(TAG, "Desired state set to %s", LOG_STR_ARG(voice_assistant_state_to_string(desired_state)));
}

void VoiceAssistant::failed_to_start() {
  ESP_LOGE(TAG, "Failed to start server. See Home Assistant logs for more details.");
  this->error_trigger_->trigger("failed-to-start", "Failed to start server. See Home Assistant logs for more details.");
  this->set_state_(State::STOP_MICROPHONE, State::IDLE);
}

void VoiceAssistant::start_streaming(struct sockaddr_storage *addr, uint16_t port) {
  if (this->state_ != State::STARTING_PIPELINE) {
    this->signal_stop_();
    return;
  }

  ESP_LOGD(TAG, "Client started, streaming microphone");

  memcpy(&this->dest_addr_, addr, sizeof(this->dest_addr_));
  if (this->dest_addr_.ss_family == AF_INET) {
    ((struct sockaddr_in *) &this->dest_addr_)->sin_port = htons(port);
  }
#if LWIP_IPV6
  else if (this->dest_addr_.ss_family == AF_INET6) {
    ((struct sockaddr_in6 *) &this->dest_addr_)->sin6_port = htons(port);
  }
#endif
  else {
    ESP_LOGW(TAG, "Unknown address family: %d", this->dest_addr_.ss_family);
    return;
  }

  if (this->mic_->is_running()) {
    this->set_state_(State::STREAMING_MICROPHONE, State::STREAMING_MICROPHONE);
  } else {
    this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
  }
}

void VoiceAssistant::request_start(bool continuous, bool silence_detection) {
  if (this->api_client_ == nullptr) {
    ESP_LOGE(TAG, "No API client connected");
    this->set_state_(State::IDLE, State::IDLE);
    this->continuous_ = false;
    return;
  }
  if (this->state_ == State::IDLE) {
    this->continuous_ = continuous;
    this->silence_detection_ = silence_detection;
#ifdef USE_ESP_ADF
    if (this->use_wake_word_) {
      rb_reset(this->ring_buffer_);
      this->set_state_(State::START_MICROPHONE, State::WAIT_FOR_VAD);
    } else if (this->use_local_wake_word_) {
      rb_reset(this->ring_buffer_);
      this->set_state_(State::START_MICROPHONE, State::WAIT_FOR_WAKE_WORD);
    } else
#endif
    {
      { this->set_state_(State::START_PIPELINE, State::START_MICROPHONE); }
    }
  }
}

void VoiceAssistant::request_stop() {
  this->continuous_ = false;

  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
    case State::STARTING_MICROPHONE:
    case State::WAIT_FOR_WAKE_WORD:
    case State::WAITING_FOR_WAKE_WORD:
    case State::WAIT_FOR_VAD:
    case State::WAITING_FOR_VAD:
    case State::START_PIPELINE:
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STARTING_PIPELINE:
    case State::STREAMING_MICROPHONE:
      this->signal_stop_();
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STOP_MICROPHONE:
    case State::STOPPING_MICROPHONE:
      this->desired_state_ = State::IDLE;
      break;
    case State::AWAITING_RESPONSE:
    case State::STREAMING_RESPONSE:
    case State::RESPONSE_FINISHED:
      break;  // Let the incoming audio stream finish then it will go to idle.
  }
}

void VoiceAssistant::signal_stop_() {
  memset(&this->dest_addr_, 0, sizeof(this->dest_addr_));
  if (this->api_client_ == nullptr) {
    return;
  }
  ESP_LOGD(TAG, "Signaling stop...");
  api::VoiceAssistantRequest msg;
  msg.start = false;
  this->api_client_->send_voice_assistant_request(msg);
}

void VoiceAssistant::on_event(const api::VoiceAssistantEventResponse &msg) {
  ESP_LOGD(TAG, "Event Type: %d", msg.event_type);
  switch (msg.event_type) {
    case api::enums::VOICE_ASSISTANT_RUN_START:
      ESP_LOGD(TAG, "Assist Pipeline running");
      this->defer([this]() { this->start_trigger_->trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_WAKE_WORD_START:
      break;
    case api::enums::VOICE_ASSISTANT_WAKE_WORD_END: {
      ESP_LOGD(TAG, "Wake word detected");
      this->defer([this]() { this->wake_word_detected_trigger_->trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_STT_START:
      ESP_LOGD(TAG, "STT started");
      this->defer([this]() { this->listening_trigger_->trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_STT_END: {
      std::string text;
      for (auto arg : msg.data) {
        if (arg.name == "text") {
          text = std::move(arg.value);
        }
      }
      if (text.empty()) {
        ESP_LOGW(TAG, "No text in STT_END event");
        return;
      }
      ESP_LOGD(TAG, "Speech recognised as: \"%s\"", text.c_str());
      this->defer([this, text]() { this->stt_end_trigger_->trigger(text); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_INTENT_START:
      ESP_LOGD(TAG, "Intent started");
      this->defer([this]() { this->intent_start_trigger_->trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_INTENT_END: {
      for (auto arg : msg.data) {
        if (arg.name == "conversation_id") {
          this->conversation_id_ = std::move(arg.value);
        }
      }
      this->defer([this]() { this->intent_end_trigger_->trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_START: {
      std::string text;
      for (auto arg : msg.data) {
        if (arg.name == "text") {
          text = std::move(arg.value);
        }
      }
      if (text.empty()) {
        ESP_LOGW(TAG, "No text in TTS_START event");
        return;
      }
      ESP_LOGD(TAG, "Response: \"%s\"", text.c_str());
      this->defer([this, text]() {
        this->tts_start_trigger_->trigger(text);
#ifdef USE_SPEAKER
        this->speaker_->start();
#endif
      });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_END: {
      std::string url;
      for (auto arg : msg.data) {
        if (arg.name == "url") {
          url = std::move(arg.value);
        }
      }
      if (url.empty()) {
        ESP_LOGW(TAG, "No url in TTS_END event");
        return;
      }
      ESP_LOGD(TAG, "Response URL: \"%s\"", url.c_str());
      this->defer([this, url]() {
#ifdef USE_MEDIA_PLAYER
        if (this->media_player_ != nullptr) {
          this->media_player_->make_call().set_media_url(url).perform();
        }
#endif
        this->tts_end_trigger_->trigger(url);
      });
      State new_state = this->local_output_ ? State::STREAMING_RESPONSE : State::IDLE;
      this->set_state_(new_state, new_state);
      break;
    }
    case api::enums::VOICE_ASSISTANT_RUN_END: {
      ESP_LOGD(TAG, "Assist Pipeline ended");
      if (this->state_ == State::STREAMING_MICROPHONE) {
#ifdef USE_ESP_ADF
        if (this->use_wake_word_) {
          rb_reset(this->ring_buffer_);
          // No need to stop the microphone since we didn't use the speaker
          this->set_state_(State::WAIT_FOR_VAD, State::WAITING_FOR_VAD);
        } else if (this->use_local_wake_word_) {
          rb_reset(this->ring_buffer_);
          // No need to stop the microphone since we didn't use the speaker
          this->set_state_(State::WAIT_FOR_WAKE_WORD, State::WAITING_FOR_WAKE_WORD);
        } else
#endif
        {
          this->set_state_(State::IDLE, State::IDLE);
        }
      }
      this->defer([this]() { this->end_trigger_->trigger(); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_ERROR: {
      std::string code = "";
      std::string message = "";
      for (auto arg : msg.data) {
        if (arg.name == "code") {
          code = std::move(arg.value);
        } else if (arg.name == "message") {
          message = std::move(arg.value);
        }
      }
      if (code == "wake-word-timeout" || code == "wake_word_detection_aborted") {
        // Don't change state here since either the "tts-end" or "run-end" events will do it.
        return;
      } else if (code == "wake-provider-missing" || code == "wake-engine-missing") {
        // Wake word is not set up or not ready on Home Assistant so stop and do not retry until user starts again.
        this->defer([this, code, message]() {
          this->request_stop();
          this->error_trigger_->trigger(code, message);
        });
        return;
      }
      ESP_LOGE(TAG, "Error: %s - %s", code.c_str(), message.c_str());
      if (this->state_ != State::IDLE) {
        this->signal_stop_();
        this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      }
      this->defer([this, code, message]() { this->error_trigger_->trigger(code, message); });
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_STREAM_START: {
#ifdef USE_SPEAKER
      this->wait_for_stream_end_ = true;
      ESP_LOGD(TAG, "TTS stream start");
      this->defer([this] { this->tts_stream_start_trigger_->trigger(); });
#endif
      break;
    }
    case api::enums::VOICE_ASSISTANT_TTS_STREAM_END: {
#ifdef USE_SPEAKER
      this->stream_ended_ = true;
      ESP_LOGD(TAG, "TTS stream end");
#endif
      break;
    }
    case api::enums::VOICE_ASSISTANT_STT_VAD_START:
      ESP_LOGD(TAG, "Starting STT by VAD");
      this->defer([this]() { this->stt_vad_start_trigger_->trigger(); });
      break;
    case api::enums::VOICE_ASSISTANT_STT_VAD_END:
      ESP_LOGD(TAG, "STT by VAD end");
      this->set_state_(State::STOP_MICROPHONE, State::AWAITING_RESPONSE);
      this->defer([this]() { this->stt_vad_end_trigger_->trigger(); });
      break;
    default:
      ESP_LOGD(TAG, "Unhandled event type: %d", msg.event_type);
      break;
  }
}

VoiceAssistant *global_voice_assistant = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace voice_assistant
}  // namespace esphome

#endif  // USE_VOICE_ASSISTANT
