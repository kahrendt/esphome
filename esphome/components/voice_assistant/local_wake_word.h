#pragma once

#include "esphome/core/helpers.h"

#ifdef USE_ESP_ADF
#include <esp_vad.h>
#include <ringbuf.h>
#endif

#include "audio_preprocessor_float32_model_data.h"
#include "model.h"

#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

namespace esphome {
namespace voice_assistant {

static const char *const TAG_LOCAL = "local_wake_word";

using AudioPreprocessorOpResolver = tflite::MicroMutableOpResolver<18>;

class LocalWakeWord {
 public:
  bool intialize_models() {
    ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->streaming_tensor_arena_ = arena_allocator.allocate(streaming_model_arena_size_);
    if (this->streaming_tensor_arena_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    this->nonstreaming_tensor_arena_ = arena_allocator.allocate(nonstreaming_model_arena_size_);
    if (this->nonstreaming_tensor_arena_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    this->streaming_var_arena_ = arena_allocator.allocate(streaming_var_arena_size_);
    if (this->streaming_var_arena_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    this->g_arena_ = arena_allocator.allocate(kArenaSize);
    if (this->g_arena_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    ExternalRAMAllocator<float> feature_buffer_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
    this->feature_buffer_ = feature_buffer_allocator.allocate(kFeatureElementCount);
    if (this->feature_buffer_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    ExternalRAMAllocator<int16_t> audio_output_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
    this->g_audio_output_buffer_ = audio_output_allocator.allocate(kMaxAudioSampleSize * 32);
    if (this->g_audio_output_buffer_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }
    ExternalRAMAllocator<int16_t> history_buffer_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
    this->g_history_buffer_ = history_buffer_allocator.allocate(HISTORY_SAMPLES_TO_KEEP);
    if (this->g_history_buffer_ == nullptr) {
      ESP_LOGW(TAG_LOCAL, "Could not allocate send buffer.");
      return false;
    }

    this->preprocessor_model_ = tflite::GetModel(g_audio_preprocessor_float32_tflite);
    if (this->preprocessor_model_->version() != TFLITE_SCHEMA_VERSION) {
      return false;
    }

    // Map the model into a usable data structure. This doesn't involve any
    // copying or parsing, it's a very lightweight operation.
    this->streaming_model_ = tflite::GetModel(streaming_model);
    if (this->streaming_model_->version() != TFLITE_SCHEMA_VERSION) {
      MicroPrintf("Model provided is schema version %d not equal to supported "
                  "version %d.",
                  this->streaming_model_->version(), TFLITE_SCHEMA_VERSION);
      return false;
    }
    this->nonstreaming_model_ = tflite::GetModel(nonstreaming_model);
    if (this->nonstreaming_model_->version() != TFLITE_SCHEMA_VERSION) {
      MicroPrintf("Model provided is schema version %d not equal to supported "
                  "version %d.",
                  this->streaming_model_->version(), TFLITE_SCHEMA_VERSION);
      return false;
    }

    static tflite::MicroMutableOpResolver<18> preprocessor_op_resolver;
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddReshape());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddCast());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddStridedSlice());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddConcatenation());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddMul());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddAdd());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddDiv());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddMinimum());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddMaximum());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddWindow());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddFftAutoScale());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddRfft());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddEnergy());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddFilterBank());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddFilterBankSquareRoot());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddFilterBankSpectralSubtraction());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddPCAN());
    TF_LITE_ENSURE_STATUS(preprocessor_op_resolver.AddFilterBankLog());

    static tflite::MicroInterpreter static_preprocessor_interpreter(this->preprocessor_model_, preprocessor_op_resolver,
                                                                    this->g_arena_, kArenaSize);
    this->preprocessor_interperter_ = &static_preprocessor_interpreter;

    if (this->preprocessor_interperter_->AllocateTensors() != kTfLiteOk) {
      return false;
    }

    // Pull in only the operation implementations we need.
    // This relies on a complete list of all the ops needed by this graph.
    // An easier approach is to just use the AllOpsResolver, but this will
    // incur some penalty in code space for op implementations that are not
    // needed by this graph.
    //
    static tflite::MicroMutableOpResolver<21> micro_op_resolver;
    if (micro_op_resolver.AddCallOnce() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddVarHandle() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddReadVariable() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddFullyConnected() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddPad() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddStridedSlice() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddConcatenation() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddAssignVariable() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddReshape() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddSoftmax() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddConv2D() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddAdd() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddLog() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddMaximum() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddGather() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddSqrt() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddMul() != kTfLiteOk) {
      return false;
    }

    if (micro_op_resolver.AddMean() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddLogistic() != kTfLiteOk) {
      return false;
    }
    if (micro_op_resolver.AddAveragePool2D() != kTfLiteOk) {
      return false;
    }

    tflite::MicroAllocator *ma = tflite::MicroAllocator::Create(this->streaming_var_arena_, streaming_var_arena_size_);
    tflite::MicroResourceVariables *mrv = tflite::MicroResourceVariables::Create(ma, 8);

    // Build an interpreter to run the model with.static
    static tflite::MicroInterpreter static_streaming_interpreter(
        this->streaming_model_, micro_op_resolver, this->streaming_tensor_arena_, streaming_model_arena_size_, mrv);

    static tflite::MicroInterpreter static_nonstreaming_interpreter(
        this->nonstreaming_model_, micro_op_resolver, this->nonstreaming_tensor_arena_, nonstreaming_model_arena_size_);
    this->streaming_interpreter_ = &static_streaming_interpreter;
    this->nonstreaming_interpreter_ = &static_nonstreaming_interpreter;

    // Allocate memory from the streaming_tensor_arena_ for the model's tensors.
    TfLiteStatus allocate_status = this->streaming_interpreter_->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "AllocateTensors() failed for streaming interpeter");
      return false;
    } else {
      ESP_LOGD(TAG_LOCAL, "AllocateTEnsors() succeeded for streaming interpreter");
    }
    allocate_status = this->nonstreaming_interpreter_->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "AllocateTensors() failed for nonstreaming interpeter");
      return false;
    } else {
      ESP_LOGD(TAG_LOCAL, "AllocateTEnsors() succeeded for nonstreaming interpreter");
    }

    this->streaming_model_input_ = tflite::GetTensorData<float>(this->streaming_interpreter_->input(0));
    this->nonstreaming_model_input_ = tflite::GetTensorData<float>(this->nonstreaming_interpreter_->input(0));

    for (int n = 0; n < kFeatureElementCount; ++n) {
      this->feature_buffer_[n] = 0.0;
    }

    this->previous_time_ = 0;

    return true;
  }

  TfLiteStatus populate_feature_data(int *how_many_new_slices, ringbuf_handle_t &ring_buffer) {
    // Quantize the time into steps as long as each window stride, so we can
    // figure out which audio data we need to fetch.
    // const int last_step = (last_time_in_ms / kFeatureStrideMs);
    // const int current_step = (time_in_ms / kFeatureStrideMs);

    // int slices_needed = current_step - last_step;
    // If this is the first call, make sure we don't use any cached information.
    int slices_needed = rb_bytes_filled(ring_buffer) / (NEW_SAMPLES_TO_GET * sizeof(int16_t));
    *how_many_new_slices = slices_needed;

    if (slices_needed > kFeatureCount) {
      slices_needed = kFeatureCount;
    }
    *how_many_new_slices = slices_needed;

    const int slices_to_keep = kFeatureCount - slices_needed;
    const int slices_to_drop = kFeatureCount - slices_to_keep;
    // If we can avoid recalculating some slices, just move the existing data
    // up in the spectrogram, to perform something like this:
    // last time = 80ms          current time = 120ms
    // +-----------+             +-----------+
    // | data@20ms |         --> | data@60ms |
    // +-----------+       --    +-----------+
    // | data@40ms |     --  --> | data@80ms |
    // +-----------+   --  --    +-----------+
    // | data@60ms | --  --      |  <empty>  |
    // +-----------+   --        +-----------+
    // | data@80ms | --          |  <empty>  |
    // +-----------+             +-----------+
    if (slices_to_keep > 0) {
      for (int dest_slice = 0; dest_slice < slices_to_keep; ++dest_slice) {
        float *dest_slice_data = this->feature_buffer_ + (dest_slice * kFeatureSize);
        const int src_slice = dest_slice + slices_to_drop;
        const float *src_slice_data = this->feature_buffer_ + (src_slice * kFeatureSize);
        for (int i = 0; i < kFeatureSize; ++i) {
          dest_slice_data[i] = src_slice_data[i];
        }
      }
    }

    // Any slices that need to be filled in with feature data have their
    // appropriate audio data pulled, and features calculated for that slice.
    if (slices_needed > 0) {
      for (int new_slice = slices_to_keep; new_slice < kFeatureCount; ++new_slice) {
        // const int new_step = (current_step - kFeatureCount + 1) + new_slice;
        // const int32_t slice_start_ms = (new_step * kFeatureStrideMs);
        int16_t *audio_samples = nullptr;
        int audio_samples_size = 0;

        if (this->stride_audio_samples_(&audio_samples_size, &audio_samples, ring_buffer) != kTfLiteOk) {
          return kTfLiteError;
        }
        if (audio_samples_size < kMaxAudioSampleSize) {
          ESP_LOGD(TAG_LOCAL, "Audio data size %d too small, want %d", audio_samples_size, kMaxAudioSampleSize);
          return kTfLiteError;
        }
        static constexpr int kAudioSampleDurationCount = kFeatureDurationMs * kAudioSampleFrequency / 1000;

        float *new_slice_data = this->feature_buffer_ + (new_slice * kFeatureSize);

        if (this->features_count_ < 49) {
          ++this->features_count_;
        }

        TfLiteStatus generate_status =
            GenerateSingleFloatFeature(audio_samples, kAudioSampleDurationCount, new_slice_data);
        if (generate_status != kTfLiteOk) {
          return generate_status;
        }

        for (int i = 0; i < kFeatureSize; ++i) {
          this->streaming_model_input_[i] = new_slice_data[i];
        }

        uint32_t prior_invoke = millis();
        // Run the model on the spectrogram input and make sure it succeeds.
        TfLiteStatus invoke_status = this->streaming_interpreter_->Invoke();
        if (invoke_status != kTfLiteOk) {
          ESP_LOGD(TAG_LOCAL, "Invoke failed");
          return kTfLiteError;
        }
        ESP_LOGV(TAG_LOCAL, "Streaming Inference Latency=%u ms", (millis() - prior_invoke));

        TfLiteTensor *output = this->streaming_interpreter_->output(0);

        float max_result = 0.0;
        int max_idx = 0;
        for (int i = 0; i < kCategoryCount; i++) {
          float current_result = tflite::GetTensorData<float>(output)[i];
          if (current_result > max_result) {
            max_result = current_result;  // update max result
            max_idx = i;                  // update category
          }
        }

        if ((max_result > 0.8f) && (max_idx == 2) && (this->features_count_ > 30)) {
          ++this->succesive_wake_words;
        } else {
          if (this->succesive_wake_words > 0) {
            --this->succesive_wake_words;
          }
        }
        // if ((max_result > 0.95f) && (max_idx == 2)) {
        //   if (this->last_probability > 0.95f) {
        //     ++this->succesive_wake_words;
        //   }
        //   this->last_probability = max_result;
        // } else {
        //   this->last_probability = 0.0;
        //   this->succesive_wake_words = 0;
        // }

        // if (millis() - this->last_wake_word_check_ > 100) {
        // if ((tflite::GetTensorData<float>(output)[2] > 0.5)) {
        ESP_LOGD(TAG_LOCAL, "silence=%.3f,unknown=%.3f,computer=%.3f", tflite::GetTensorData<float>(output)[0],
                 tflite::GetTensorData<float>(output)[1], tflite::GetTensorData<float>(output)[2]);
        // }

        // this->last_wake_word_check_ = millis();
        // }

        // // // if (max_result > 0.8f) {
        // ESP_LOGD(TAG_LOCAL, "Detected %7s, score: %.5f", kCategoryLabels[max_idx], static_cast<double>(max_result));
      }
    }

    return kTfLiteOk;
  }

 protected:
  const tflite::Model *streaming_model_ = nullptr;
  tflite::MicroInterpreter *streaming_interpreter_ = nullptr;
  const tflite::Model *nonstreaming_model_ = nullptr;
  tflite::MicroInterpreter *nonstreaming_interpreter_ = nullptr;
  const tflite::Model *preprocessor_model_{nullptr};
  tflite::MicroInterpreter *preprocessor_interperter_{nullptr};

  int32_t previous_time_ = 0;

  // Create an area of memory to use for input, output, and intermediate arrays.
  // The size of this will depend on the model you're using, and may need to be
  // determined by experimentation.
  static constexpr int streaming_model_arena_size_ = 1 * 1024 * 1000;
  static constexpr int streaming_var_arena_size_ = 1 * 10 * 1000;
  static constexpr int nonstreaming_model_arena_size_ = 1 * 1024 * 1000;

  uint8_t *streaming_tensor_arena_{nullptr};
  uint8_t *nonstreaming_tensor_arena_{nullptr};
  uint8_t *streaming_var_arena_{nullptr};
  uint8_t *g_arena_{nullptr};
  float *feature_buffer_{nullptr};
  float *streaming_model_input_{nullptr};
  float *nonstreaming_model_input_{nullptr};

  float output_probabilities_[3];

  // Make sure we don't try to use cached information if this is the first call
  // into the provider.
  bool is_first_run_{true};

  // // Fills the feature data with information from audio inputs, and returns how
  // // many feature slices were updated.
  // TfLiteStatus populate_feature_data_(int32_t last_time_in_ms, int32_t time_in_ms, int *how_many_new_slices);

  // // This is an abstraction around an audio source like a microphone, and is
  // // expected to return 16-bit PCM sample data for a given point in time. The
  // // sample data itself should be used as quickly as possible by the caller, since
  // // to allow memory optimizations there are no guarantees that the samples won't
  // // be overwritten by new data in the future. In practice, implementations should
  // // ensure that there's a reasonable time allowed for clients to access the data
  // // before any reuse.
  // // The reference implementation can have no platform-specific dependencies, so
  // // it just returns an array filled with zeros. For real applications, you should
  // // ensure there's a specialized implementation that accesses hardware APIs.
  // TfLiteStatus stride_audio_samples_(int *audio_samples_size, int16_t **audio_samples);

  // Returns the time that audio data was last captured in milliseconds. There's
  // no contract about what time zero represents, the accuracy, or the granularity
  // of the result. Subsequent calls will generally not return a lower value, but
  // even that's not guaranteed if there's an overflow wraparound.
  // The reference implementation of this function just returns a constantly
  // incrementing value for each call, since it would need a non-portable platform
  // call to access time information. For real applications, you'll need to write
  // your own platform-specific implementation.
  int32_t latest_audio_timestamp_() { return g_latest_audio_timestamp; }

  /* ringbuffer to hold the incoming audio data */
  // ringbuf_t *g_audio_capture_buffer;
  int32_t g_latest_audio_timestamp = 0;

  // Stores audio fed into feature generator
  int16_t *g_audio_output_buffer_;
  int16_t *g_history_buffer_;

  uint8_t succesive_wake_words = 0;

  uint8_t features_count_ = 0;

  // The following values are derived from values used during model training.
  // If you change the way you preprocess the input, update all these constants.
  int kMaxAudioSampleSize = 512;
  static constexpr int kAudioSampleFrequency = 16000;
  static constexpr int kFeatureSize = 40;
  int kFeatureCount = 49;
  int kFeatureElementCount = (kFeatureSize * kFeatureCount);
  int kFeatureStrideMs = 20;
  static constexpr int kFeatureDurationMs = 30;

  /* model requires 20ms new data from g_audio_capture_buffer and 10ms old data
   * each time , storing old data in the histrory buffer , {
   * history_samples_to_keep = 10 * 16 } */
  int32_t HISTORY_SAMPLES_TO_KEEP = ((kFeatureDurationMs - kFeatureStrideMs) * (kAudioSampleFrequency / 1000));
  /* new samples to get each time from ringbuffer, { new_samples_to_get =  20 * 16
   * } */
  int32_t NEW_SAMPLES_TO_GET = (kFeatureStrideMs * (kAudioSampleFrequency / 1000));

  // Variables for the model's output categories.
  static constexpr int kCategoryCount = 3;
  const char *kCategoryLabels[kCategoryCount] = {
      "silence",
      "unknown",
      "computer",
  };

  const tflite::Model *model = nullptr;
  tflite::MicroInterpreter *interpreter = nullptr;

  static constexpr size_t kArenaSize = 16 * 1024;

  int kAudioSampleDurationCount = kFeatureDurationMs * kAudioSampleFrequency / 1000;
  int kAudioSampleStrideCount = kFeatureStrideMs * kAudioSampleFrequency / 1000;

  TfLiteStatus stride_audio_samples_(int *audio_samples_size, int16_t **audio_samples, ringbuf_handle_t &ring_buffer) {
    /* copy 160 samples (320 bytes) into output_buff from history */
    memcpy((void *) (this->g_audio_output_buffer_), (void *) (this->g_history_buffer_),
           HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

    /* copy 320 samples (640 bytes) from rb at ( int16_t*(g_audio_output_buffer) +
     * 160 ), first 160 samples (320 bytes) will be from history */
    if (rb_bytes_filled(ring_buffer) < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
      ESP_LOGD(TAG_LOCAL, " Buffer not full enough");
      return kTfLiteError;
    }
    int bytes_read = rb_read(ring_buffer, ((char *) (this->g_audio_output_buffer_ + HISTORY_SAMPLES_TO_KEEP)),
                             NEW_SAMPLES_TO_GET * sizeof(int16_t), pdMS_TO_TICKS(200));
    if (bytes_read < 0) {
      ESP_LOGE(TAG_LOCAL, " Model Could not read data from Ring Buffer");
    } else if (bytes_read < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
      ESP_LOGD(TAG_LOCAL, " Partial Read of Data by Model");
      ESP_LOGD(TAG_LOCAL, " Could only read %d bytes when required %d bytes ", bytes_read,
               (int) (NEW_SAMPLES_TO_GET * sizeof(int16_t)));
      return kTfLiteError;
    }

    /* copy 320 bytes from output_buff into history */
    memcpy((void *) (this->g_history_buffer_), (void *) (this->g_audio_output_buffer_ + NEW_SAMPLES_TO_GET),
           HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

    *audio_samples_size = kMaxAudioSampleSize;
    // *audio_samples_size = bytes_read / sizeof(int16_t);
    *audio_samples = this->g_audio_output_buffer_;
    return kTfLiteOk;
  }

  TfLiteStatus RegisterOps(AudioPreprocessorOpResolver &op_resolver) { return kTfLiteOk; }

  TfLiteStatus InitializeMicroFeatures() {
    // g_is_first_time = true;

    // // Map the model into a usable data structure. This doesn't involve any
    // // copying or parsing, it's a very lightweight operation.
    // model = tflite::GetModel(g_audio_preprocessor_float32_tflite);
    // if (model->version() != TFLITE_SCHEMA_VERSION) {
    //   MicroPrintf("Model provided for Feature generator is schema version %d "
    //               "not equal to supported version %d.",
    //               model->version(), TFLITE_SCHEMA_VERSION);
    //   return kTfLiteError;
    // }

    // static AudioPreprocessorOpResolver op_resolver;
    // RegisterOps(op_resolver);

    // // MicroPrintf("AudioPreprocessor model arena size = %u",
    // //             interpreter.arena_used_bytes());

    return kTfLiteOk;
  }

  TfLiteStatus GenerateSingleFloatFeature(const int16_t *audio_data, const int audio_data_size,
                                          float feature_output[kFeatureSize]) {
    TfLiteTensor *input = this->preprocessor_interperter_->input(0);
    TfLiteTensor *output = this->preprocessor_interperter_->output(0);
    std::copy_n(audio_data, audio_data_size, tflite::GetTensorData<int16_t>(input));

    if (this->preprocessor_interperter_->Invoke() != kTfLiteOk) {
      MicroPrintf("Feature generator model invocation failed");
    }

    std::memcpy(feature_output, tflite::GetTensorData<float>(output), kFeatureSize * sizeof(float));

    return kTfLiteOk;
  }
};
}  // namespace voice_assistant
}  // namespace esphome
