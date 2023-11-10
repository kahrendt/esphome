#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model.h"

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
    model = tflite::GetModel(g_model);

    if (model->version() != TFLITE_SCHEMA_VERSION) {
      MicroPrintf("Model provided is schema version %d not equal to supported "
                  "version %d.",
                  model->version(), TFLITE_SCHEMA_VERSION);
      this->mark_failed();
      return;
    }

    // Pull in only the operation implementations we need.
    static tflite::MicroMutableOpResolver<1> resolver;
    if (resolver.AddFullyConnected() != kTfLiteOk) {
      return;
    }

    // Build an interpreter to run the model with.
    static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, kTensorArenaSize);
    // static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
      // MicroPrintf("AllocateTensors() failed");
      this->mark_failed();
      return;
    }

    // Obtain pointers to the model's input and output tensors.
    input = interpreter->input(0);
    output = interpreter->output(0);

    if ((input == nullptr) || (output == nullptr)) {
      this->mark_failed();
    }

    // Keep track of how many inferences we have performed.
    inference_count = 0;
  }

  void loop() {
    // Calculate an x value to feed into the model. We compare the current
    // inference_count to the number of inferences per cycle to determine
    // our position within the range of possible x values the model was
    // trained on, and use this to calculate a value.
    float position = static_cast<float>(inference_count) / static_cast<float>(kInferencesPerCycle);
    float x = position * 2.f * 3.14159265359f;
    // float x = position * kXrange;

    // Quantize the input from floating-point to integer
    int8_t x_quantized = x / input->params.scale + input->params.zero_point;
    // x_quantized = -34;

    // ESP_LOGD(TAG, "input->params.scale=%.2f;zer_point=%d", input->params.scale, input->params.zero_point);
    // Place the quantized input in the model's input tensor
    input->data.int8[0] = x_quantized;

    // Run inference, and report any error
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
      // MicroPrintf("Invoke failed on x: %f\n", static_cast<double>(x));
      ESP_LOGE(TAG, "Invoke failed on x");
      return;
    }

    // Obtain the quantized output from model's output tensor
    int8_t y_quantized = output->data.int8[0];
    ESP_LOGD(TAG, "x_quantized=%d; y_quantized=%d", x_quantized, y_quantized);
    // Dequantize the output from integer to floating-point
    float y = (y_quantized - output->params.zero_point) * output->params.scale;
    ESP_LOGD(TAG, "output->params.scale=%.2f;zer_point=%d", output->params.scale, output->params.zero_point);

    // Output the results. A custom HandleOutput function can be implemented
    // for each supported hardware target.
    // HandleOutput(x, y);
    ESP_LOGD(TAG, "x=%.2f;y=%.2f", x, y);

    // Increment the inference_counter, and reset it if we have reached
    // the total number per cycle
    inference_count += 1;
    if (inference_count >= kInferencesPerCycle)
      inference_count = 0;
  }

 protected:
  const tflite::Model *model{nullptr};
  tflite::MicroInterpreter *interpreter{nullptr};
  TfLiteTensor *input{nullptr};
  TfLiteTensor *output{nullptr};
  int inference_count{0};
  const int kInferencesPerCycle{20};

  static constexpr int kTensorArenaSize{2000};
  uint8_t tensor_arena[kTensorArenaSize];
};

}  // namespace tflite2
}  // namespace esphome
