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

// Constants used for audio preprocessor model
enum {
  PREPROCESSOR_FEATURE_SIZE = 40,   // The number of features the audio preprocessor generates per slice
  PREPROCESSOR_FEATURE_COUNT = 49,  // The number of slices in the spectrogram
  FEATURE_STRIDE_MS = 20,           // How frequently the preprocessor generates a new set of features
  FEATURE_DURATION_MS = 30,         // Duration of each slice used as input into the preprocessor
  AUDIO_SAMPLE_FREQUENCY = 16000,   // Audio sample frequency in hertz
  SPECTROGRAM_PIXELS = (PREPROCESSOR_FEATURE_SIZE * PREPROCESSOR_FEATURE_COUNT),
  HISTORY_SAMPLES_TO_KEEP = ((FEATURE_DURATION_MS - FEATURE_STRIDE_MS) * (AUDIO_SAMPLE_FREQUENCY / 1000)),
  NEW_SAMPLES_TO_GET = (FEATURE_STRIDE_MS * (AUDIO_SAMPLE_FREQUENCY / 1000)),
  SAMPLE_DURATION_COUNT = FEATURE_DURATION_MS * AUDIO_SAMPLE_FREQUENCY / 1000,
  MAX_AUDIO_SAMPLE_SIZE = 512,
};

// Constants used for setting up tensor arenas
// TODO: Optimize these values; they are currently much larger than needed
enum {
  STREAMING_MODEL_ARENA_SIZE = 1024 * 1000,
  STREAMING_MODEL_VARIABLE_ARENA_SIZE = 10 * 1000,
  NONSTREAMING_MODEL_ARENA_SIZE = 1024 * 1000,
  PREPROCESSOR_ARENA_SIZE = 16 * 1024,
};

enum {
  STREAMING_MODEL_CATEGORIES = 3,
};

class LocalWakeWord {
 public:
  bool intialize_models() {
    ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    ExternalRAMAllocator<float> spectrogram_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
    ExternalRAMAllocator<int16_t> audio_samples_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);

    this->streaming_tensor_arena_ = arena_allocator.allocate(STREAMING_MODEL_ARENA_SIZE);
    if (this->streaming_tensor_arena_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the streaming model's tensor arena.");
      return false;
    }

    this->nonstreaming_tensor_arena_ = arena_allocator.allocate(NONSTREAMING_MODEL_ARENA_SIZE);
    if (this->nonstreaming_tensor_arena_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the nonstreaming model's tensor arena.");
      return false;
    }

    this->streaming_var_arena_ = arena_allocator.allocate(STREAMING_MODEL_VARIABLE_ARENA_SIZE);
    if (this->streaming_var_arena_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the streaming model variable's tensor arena.");
      return false;
    }

    this->preprocessor_tensor_arena_ = arena_allocator.allocate(PREPROCESSOR_ARENA_SIZE);
    if (this->preprocessor_tensor_arena_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the audio preprocessor model's tensor arena.");
      return false;
    }

    this->spectrogram_ = spectrogram_allocator.allocate(SPECTROGRAM_PIXELS);
    if (this->spectrogram_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the audio features buffer.");
      return false;
    }

    this->preprocessor_audio_buffer_ = audio_samples_allocator.allocate(MAX_AUDIO_SAMPLE_SIZE * 32);
    if (this->preprocessor_audio_buffer_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the audio preprocessor's buffer.");
      return false;
    }

    this->preprocessor_stride_buffer_ = audio_samples_allocator.allocate(HISTORY_SAMPLES_TO_KEEP);
    if (this->preprocessor_stride_buffer_ == nullptr) {
      ESP_LOGE(TAG_LOCAL, "Could not allocate the audio preprocessor's stride buffer.");
      return false;
    }

    this->preprocessor_model_ = tflite::GetModel(g_audio_preprocessor_float32_tflite);
    if (this->preprocessor_model_->version() != TFLITE_SCHEMA_VERSION) {
      ESP_LOGE(TAG_LOCAL, "Wake word's audio preprocessor model's schema is not supported");
      return false;
    }

    this->streaming_model_ = tflite::GetModel(streaming_model);
    if (this->streaming_model_->version() != TFLITE_SCHEMA_VERSION) {
      ESP_LOGE(TAG_LOCAL, "Wake word's streaming model's schema is not supported");
      return false;
    }
    this->nonstreaming_model_ = tflite::GetModel(nonstreaming_model);
    if (this->nonstreaming_model_->version() != TFLITE_SCHEMA_VERSION) {
      ESP_LOGE(TAG_LOCAL, "Wake word's nonstreaming model's schema is not supported");
      return false;
    }

    static tflite::MicroMutableOpResolver<18> preprocessor_op_resolver;
    static tflite::MicroMutableOpResolver<8> nonstreaming_op_resolver;
    static tflite::MicroMutableOpResolver<12> streaming_op_resolver;

    if (!this->register_preprocessor_ops_(preprocessor_op_resolver))
      return false;
    if (!this->register_nonstreaming_ops_(nonstreaming_op_resolver))
      return false;
    if (!this->register_streaming_ops_(streaming_op_resolver))
      return false;

    tflite::MicroAllocator *ma =
        tflite::MicroAllocator::Create(this->streaming_var_arena_, STREAMING_MODEL_VARIABLE_ARENA_SIZE);
    tflite::MicroResourceVariables *mrv = tflite::MicroResourceVariables::Create(ma, 8);

    static tflite::MicroInterpreter static_preprocessor_interpreter(
        this->preprocessor_model_, preprocessor_op_resolver, this->preprocessor_tensor_arena_, PREPROCESSOR_ARENA_SIZE);

    static tflite::MicroInterpreter static_streaming_interpreter(
        this->streaming_model_, streaming_op_resolver, this->streaming_tensor_arena_, STREAMING_MODEL_ARENA_SIZE, mrv);

    static tflite::MicroInterpreter static_nonstreaming_interpreter(this->nonstreaming_model_, nonstreaming_op_resolver,
                                                                    this->nonstreaming_tensor_arena_,
                                                                    NONSTREAMING_MODEL_ARENA_SIZE);

    this->preprocessor_interperter_ = &static_preprocessor_interpreter;
    this->streaming_interpreter_ = &static_streaming_interpreter;
    this->nonstreaming_interpreter_ = &static_nonstreaming_interpreter;

    // Allocate tensors for each models.
    if (this->preprocessor_interperter_->AllocateTensors() != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "Failed to allocate tensors for the audio preprocessor");
      return false;
    }
    if (this->streaming_interpreter_->AllocateTensors() != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "Failed to allocate tensors for the streaming model");
      return false;
    }
    if (this->nonstreaming_interpreter_->AllocateTensors() != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "Failed to allocate tensors for the nonstreaming model");
      return false;
    }

    this->streaming_model_input_ = tflite::GetTensorData<float>(this->streaming_interpreter_->input(0));
    this->nonstreaming_model_input_ = tflite::GetTensorData<float>(this->nonstreaming_interpreter_->input(0));

    // Reset the feature buffer
    for (int n = 0; n < SPECTROGRAM_PIXELS; ++n) {
      this->spectrogram_[n] = 0.0;
    }

    return true;
  }

  bool run_inference(ringbuf_handle_t &ring_buffer) {
    this->populate_feature_data_(ring_buffer);
    if (this->succesive_wake_words >= 5) {
      ESP_LOGD(TAG_LOCAL, "Streaming model predicted the wake word");
      this->succesive_wake_words = 0;

      // Reset counter of features so we don't reduplicate our inference; the internal
      // variables for the streaming model do not reset
      this->spectrogram_current_features_count_ = 0;

      // Copy the entire spectogram as input to the nonstreaming model
      for (int i = 0; i < SPECTROGRAM_PIXELS; ++i) {
        this->nonstreaming_model_input_[i] = this->spectrogram_[i];
      }

      uint32_t prior_invoke = millis();

      // Run the model on the spectrogram input and make sure it succeeds.
      TfLiteStatus invoke_status = this->nonstreaming_interpreter_->Invoke();
      if (invoke_status != kTfLiteOk) {
        ESP_LOGD(TAG_LOCAL, "Nonstreaming model invoke failed");
        return false;
      }

      ESP_LOGV(TAG_LOCAL, "Nonstreaming inference latency=%u ms", (millis() - prior_invoke));

      TfLiteTensor *output = this->nonstreaming_interpreter_->output(0);

      ESP_LOGV(TAG, "Nonstreaming Model Predictions: silence=%.3f, unknown=%.3f, computer=%.3f",
               tflite::GetTensorData<float>(output)[0], tflite::GetTensorData<float>(output)[1],
               tflite::GetTensorData<float>(output)[2]);

      // If the nonstreaming model predicts the wake word, then return true
      if (tflite::GetTensorData<float>(output)[2] > 0.9f) {
        return true;
      }
    }
    return false;
  }

 protected:
  const tflite::Model *preprocessor_model_{nullptr};
  const tflite::Model *streaming_model_{nullptr};
  const tflite::Model *nonstreaming_model_{nullptr};
  tflite::MicroInterpreter *streaming_interpreter_{nullptr};
  tflite::MicroInterpreter *nonstreaming_interpreter_{nullptr};
  tflite::MicroInterpreter *preprocessor_interperter_{nullptr};

  uint8_t *streaming_var_arena_{nullptr};
  uint8_t *streaming_tensor_arena_{nullptr};
  uint8_t *nonstreaming_tensor_arena_{nullptr};
  uint8_t *preprocessor_tensor_arena_{nullptr};

  float *spectrogram_{nullptr};
  float *streaming_model_input_{nullptr};
  float *nonstreaming_model_input_{nullptr};

  // Stores audio fed into feature generator
  int16_t *preprocessor_audio_buffer_;
  int16_t *preprocessor_stride_buffer_;

  uint8_t succesive_wake_words = 0;

  uint8_t spectrogram_current_features_count_ = 0;

  bool populate_feature_data_(ringbuf_handle_t &ring_buffer) {
    int slices_needed = rb_bytes_filled(ring_buffer) / (NEW_SAMPLES_TO_GET * sizeof(int16_t));

    if (slices_needed > PREPROCESSOR_FEATURE_COUNT) {
      slices_needed = PREPROCESSOR_FEATURE_COUNT;
    }

    const int slices_to_keep = PREPROCESSOR_FEATURE_COUNT - slices_needed;
    const int slices_to_drop = PREPROCESSOR_FEATURE_COUNT - slices_to_keep;

    // We move the existing data up in the spectrogram, to perform something like this:
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
        float *dest_slice_data = this->spectrogram_ + (dest_slice * PREPROCESSOR_FEATURE_SIZE);
        const int src_slice = dest_slice + slices_to_drop;
        const float *src_slice_data = this->spectrogram_ + (src_slice * PREPROCESSOR_FEATURE_SIZE);
        for (int i = 0; i < PREPROCESSOR_FEATURE_SIZE; ++i) {
          dest_slice_data[i] = src_slice_data[i];
        }
      }
    }

    // Any slices that need to be filled in with feature data have their
    // appropriate audio data pulled, and features calculated for that slice.
    if (slices_needed > 0) {
      for (int new_slice = slices_to_keep; new_slice < PREPROCESSOR_FEATURE_COUNT; ++new_slice) {
        int16_t *audio_samples = nullptr;

        if (!this->stride_audio_samples_(&audio_samples, ring_buffer)) {
          return false;
        }

        float *new_slice_data = this->spectrogram_ + (new_slice * PREPROCESSOR_FEATURE_SIZE);

        if (this->spectrogram_current_features_count_ < PREPROCESSOR_FEATURE_COUNT) {
          ++this->spectrogram_current_features_count_;
        }

        TfLiteStatus generate_status =
            this->generate_single_float_feature(audio_samples, SAMPLE_DURATION_COUNT, new_slice_data);
        if (generate_status != kTfLiteOk) {
          return generate_status;
        }

        for (int i = 0; i < PREPROCESSOR_FEATURE_SIZE; ++i) {
          this->streaming_model_input_[i] = new_slice_data[i];
        }

        uint32_t prior_invoke = millis();

        // Run the model on the spectrogram input and make sure it succeeds.
        TfLiteStatus invoke_status = this->streaming_interpreter_->Invoke();
        if (invoke_status != kTfLiteOk) {
          ESP_LOGD(TAG_LOCAL, "Invoke failed");
          // return kTfLiteError;
          return false;
        }

        ESP_LOGV(TAG_LOCAL, "Streaming Inference Latency=%u ms", (millis() - prior_invoke));

        TfLiteTensor *output = this->streaming_interpreter_->output(0);

        float max_result = 0.0;
        int max_idx = 0;
        for (int i = 0; i < STREAMING_MODEL_CATEGORIES; i++) {
          float current_result = tflite::GetTensorData<float>(output)[i];
          if (current_result > max_result) {
            max_result = current_result;  // update max result
            max_idx = i;                  // update category
          }
        }

        if ((max_result > 0.8f) && (max_idx == 2) &&
            (this->spectrogram_current_features_count_ > PREPROCESSOR_FEATURE_COUNT / 2)) {
          ++this->succesive_wake_words;
        } else {
          if (this->succesive_wake_words > 0) {
            --this->succesive_wake_words;
          }
        }

        // if ((tflite::GetTensorData<float>(output)[2] > 0.5)) {
        ESP_LOGD(TAG_LOCAL, "silence=%.3f,unknown=%.3f,computer=%.3f", tflite::GetTensorData<float>(output)[0],
                 tflite::GetTensorData<float>(output)[1], tflite::GetTensorData<float>(output)[2]);
        // }
      }
    }

    return true;
  }

  // Return true if successful
  bool stride_audio_samples_(int16_t **audio_samples, ringbuf_handle_t &ring_buffer) {
    // Copy 320 bytes (160 samples over 10 ms) into preprocessor_audio_buffer_ from history in
    // preprocessor_stride_buffer_
    memcpy((void *) (this->preprocessor_audio_buffer_), (void *) (this->preprocessor_stride_buffer_),
           HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

    if (rb_bytes_filled(ring_buffer) < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
      ESP_LOGD(TAG_LOCAL, "Audio Buffer not full enough");
      return false;
    }

    // Copy 640 bytes (320 samples over 20 ms) from the ring buffer
    // The first 320 bytes (160 samples over 10 ms) will be from history
    int bytes_read = rb_read(ring_buffer, ((char *) (this->preprocessor_audio_buffer_ + HISTORY_SAMPLES_TO_KEEP)),
                             NEW_SAMPLES_TO_GET * sizeof(int16_t), pdMS_TO_TICKS(200));

    if (bytes_read < 0) {
      ESP_LOGE(TAG_LOCAL, "Could not read data from Ring Buffer");
    } else if (bytes_read < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
      ESP_LOGD(TAG_LOCAL, "Partial Read of Data by Model");
      ESP_LOGD(TAG_LOCAL, "Could only read %d bytes when required %d bytes ", bytes_read,
               (int) (NEW_SAMPLES_TO_GET * sizeof(int16_t)));
      return false;
    }

    // Copy the last 320 bytes (160 samples over 10 ms) from the audio buffer into history stride buffer for the next
    // iteration
    memcpy((void *) (this->preprocessor_stride_buffer_),
           (void *) (this->preprocessor_audio_buffer_ + NEW_SAMPLES_TO_GET), HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

    *audio_samples = this->preprocessor_audio_buffer_;
    return true;
  }

  // Return true on success
  bool register_preprocessor_ops_(tflite::MicroMutableOpResolver<18> &op_resolver) {
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

  // Return true if successful
  bool register_streaming_ops_(tflite::MicroMutableOpResolver<12> &op_resolver) {
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
    if (op_resolver.AddDepthwiseConv2D() != kTfLiteOk)
      return false;
    if (op_resolver.AddAveragePool2D() != kTfLiteOk)
      return false;
    if (op_resolver.AddFullyConnected() != kTfLiteOk)
      return false;
    if (op_resolver.AddSoftmax() != kTfLiteOk)
      return false;

    return true;
  }

  // Return true if successful
  bool register_nonstreaming_ops_(tflite::MicroMutableOpResolver<8> &op_resolver) {
    if (op_resolver.AddReshape())
      return false;
    if (op_resolver.AddConv2D())
      return false;
    if (op_resolver.AddDepthwiseConv2D())
      return false;
    if (op_resolver.AddMul())
      return false;
    if (op_resolver.AddAdd())
      return false;
    if (op_resolver.AddMean())
      return false;
    if (op_resolver.AddLogistic())
      return false;
    if (op_resolver.AddSoftmax())
      return false;

    return true;
  }

  TfLiteStatus generate_single_float_feature(const int16_t *audio_data, const int audio_data_size,
                                             float feature_output[PREPROCESSOR_FEATURE_SIZE]) {
    TfLiteTensor *input = this->preprocessor_interperter_->input(0);
    TfLiteTensor *output = this->preprocessor_interperter_->output(0);
    std::copy_n(audio_data, audio_data_size, tflite::GetTensorData<int16_t>(input));

    if (this->preprocessor_interperter_->Invoke() != kTfLiteOk) {
      ESP_LOGE(TAG_LOCAL, "Failed to preprocess audio for local wake word.");
    }

    std::memcpy(feature_output, tflite::GetTensorData<float>(output), PREPROCESSOR_FEATURE_SIZE * sizeof(float));

    return kTfLiteOk;
  }
};
}  // namespace voice_assistant
}  // namespace esphome
