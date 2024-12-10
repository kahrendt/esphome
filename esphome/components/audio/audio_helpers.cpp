#include "audio_helpers.h"

namespace esphome {
namespace audio {

void scale_audio_samples(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                         size_t samples_to_scale) {
  // Note the assembly dsps_mulc function has audio glitches if the input and output buffers are the same.
  for (int i = 0; i < samples_to_scale; i++) {
    int32_t acc = (int32_t) audio_samples[i] * (int32_t) scale_factor;
    output_buffer[i] = (int16_t) (acc >> 15);
  }
}

}  // namespace audio
}  // namespace esphome
