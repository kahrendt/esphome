#pragma once

#include <cstdint>
#include <stddef.h>

namespace esphome {
namespace audio {

/// @brief Scales audio samples. Scales in place when audio_samples == output_buffer.
/// @param audio_samples PCM int16 audio samples
/// @param output_buffer Buffer to store the scaled samples
/// @param scale_factor Q15 fixed point scaling factor
/// @param samples_to_scale Number of samples to scale
void scale_audio_samples(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor, size_t samples_to_scale);

}  // namespace audio
}  // namespace esphome
