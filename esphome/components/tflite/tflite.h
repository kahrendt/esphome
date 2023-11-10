#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "melspectrogram_model.h"
#include "embedding_model.h"

#include "esphome/core/log.h"

namespace esphome {
namespace tflite2 {

static const char *const TAG = "tflite";

class TFLiteComponent : public Component, public sensor::Sensor {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() {
    // Map the model into a usable data structure. This doesn't involve any
    // copying or parsing, it's a very lightweight operation.
    // model = tflite::GetModel(hello_world_int8_tflite);
    mel_model = tflite::GetModel(melspectrogram_tflite);
    // model = tflite::GetModel(g_model);

    embed_model = tflite::GetModel(embedding_model_tflite);

    if (mel_model->version() != TFLITE_SCHEMA_VERSION) {
      // MicroPrintf("Model provided is schema version %d not equal to supported "
      //             "version %d.",
      //             model->version(), TFLITE_SCHEMA_VERSION);
      ESP_LOGD(TAG, "model schema problem");
      this->mark_failed();
      return;
    }

    if (embed_model->version() != TFLITE_SCHEMA_VERSION) {
      // MicroPrintf("Model provided is schema version %d not equal to supported "
      //             "version %d.",
      //             model->version(), TFLITE_SCHEMA_VERSION);
      ESP_LOGD(TAG, "model schema problem");
      this->mark_failed();
      return;
    }

    // Pull in only the operation implementations we need.
    // static tflite::AllOpsResolver resolver;
    // static tflite::ops::micro::AllOpsResolver resolver;
    static tflite::MicroMutableOpResolver<12> mel_resolver;

    mel_resolver.AddExpandDims();
    mel_resolver.AddTranspose();
    mel_resolver.AddConv2D();
    mel_resolver.AddSqueeze();
    mel_resolver.AddMul();
    mel_resolver.AddAdd();
    mel_resolver.AddBatchMatMul();
    mel_resolver.AddMinimum();
    mel_resolver.AddMaximum();
    mel_resolver.AddLog();
    mel_resolver.AddReduceMax();
    mel_resolver.AddSub();

    mel_tensor_arena = (uint8_t *) heap_caps_calloc(mel_kTensorArenaSize, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Build an interpreter to run the model with.
    static tflite::MicroInterpreter static_interpreter(mel_model, mel_resolver, mel_tensor_arena, mel_kTensorArenaSize);
    // static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, kTensorArenaSize);
    mel_interpreter = &static_interpreter;

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus allocate_status = mel_interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
      // MicroPrintf("AllocateTensors() failed");
      ESP_LOGE(TAG, "couldn't allocate, allocate_status=%d", allocate_status);
      this->mark_failed();
      return;
    }

    // Obtain pointers to the model's input and output tensors.
    mel_input = mel_interpreter->input(0);
    mel_output = mel_interpreter->output(0);

    if ((mel_input == nullptr) || (mel_output == nullptr)) {
      ESP_LOGE(TAG, "nullpointers");
      this->mark_failed();
    }
  }

  void get_melspectrogram(int16_t *input) {}

  void loop() {
    float random_input[1280];

    mel_input->data.f = random_input;

    uint32_t start_time = millis();
    TfLiteStatus invoke_status = mel_interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      // MicroPrintf("Invoke failed on x: %f\n", static_cast<double>(x));
      ESP_LOGE(TAG, "Invoke failed");
      return;
    }
    ESP_LOGD(TAG, "inference time=%u", (millis() - start_time));

    // // Calculate an x value to feed into the model. We compare the current
    // // inference_count to the number of inferences per cycle to determine
    // // our position within the range of possible x values the model was
    // // trained on, and use this to calculate a value.
    // float position = static_cast<float>(inference_count) / static_cast<float>(kInferencesPerCycle);
    // float x = position * 2.f * 3.14159265359f;
    // // float x = position * kXrange;

    // // Quantize the input from floating-point to integer
    // int8_t x_quantized = x / input->params.scale + input->params.zero_point;
    // // x_quantized = -34;

    // // ESP_LOGD(TAG, "input->params.scale=%.2f;zer_point=%d", input->params.scale, input->params.zero_point);
    // // Place the quantized input in the model's input tensor
    // input->data.int8[0] = x_quantized;

    // // Run inference, and report any error
    // TfLiteStatus invoke_status = interpreter->Invoke();
    // if (invoke_status != kTfLiteOk) {
    //   // MicroPrintf("Invoke failed on x: %f\n", static_cast<double>(x));
    //   ESP_LOGE(TAG, "Invoke failed on x");
    //   return;
    // }

    // // Obtain the quantized output from model's output tensor
    // int8_t y_quantized = output->data.int8[0];
    // ESP_LOGD(TAG, "x_quantized=%d; y_quantized=%d", x_quantized, y_quantized);
    // // Dequantize the output from integer to floating-point
    // float y = (y_quantized - output->params.zero_point) * output->params.scale;
    // ESP_LOGD(TAG, "output->params.scale=%.2f;zer_point=%d", output->params.scale, output->params.zero_point);

    // // Output the results. A custom HandleOutput function can be implemented
    // // for each supported hardware target.
    // // HandleOutput(x, y);
    // ESP_LOGD(TAG, "x=%.2f;y=%.2f", x, y);

    // // Increment the inference_counter, and reset it if we have reached
    // // the total number per cycle
    // inference_count += 1;
    // if (inference_count >= kInferencesPerCycle)
    //   inference_count = 0;
  }

 protected:
  const tflite::Model *mel_model{nullptr};
  tflite::MicroInterpreter *mel_interpreter{nullptr};
  TfLiteTensor *mel_input{nullptr};
  TfLiteTensor *mel_output{nullptr};

  const tflite::Model *embed_model{nullptr};
  tflite::MicroInterpreter *embed_interpreter{nullptr};
  TfLiteTensor *embed_input{nullptr};
  TfLiteTensor *embed_output{nullptr};

  static constexpr int mel_kTensorArenaSize{1086668};
  // uint8_t tensor_arena[kTensorArenaSize];
  uint8_t *mel_tensor_arena;
};

}  // namespace tflite2
}  // namespace esphome
