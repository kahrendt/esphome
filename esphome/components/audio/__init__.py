import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE

CODEOWNERS = ["@kahrendt"]
audio_ns = cg.esphome_ns.namespace("audio")

AudioFile = audio_ns.struct("AudioFile")
AudioFileType = audio_ns.enum("AudioFileType", is_class=True)
AUDIO_FILE_TYPE_ENUM = {
    "NONE": AudioFileType.NONE,
    "WAV": AudioFileType.WAV,
    "MP3": AudioFileType.MP3,
    "FLAC": AudioFileType.FLAC,
}


CONFIG_SCHEMA = cv.All(
    cv.Schema({}),
)

AUDIO_COMPONENT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.int_range(8, 32),
        cv.Optional(CONF_NUM_CHANNELS, default=1): cv.int_range(1, 2),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(8000, 48000),
    }
)


# def final_validate_device_schema(
#     name: str,
#     *,
#     bits_per_sample: int,
#     channels: int,
#     sample_rate: int,
# ):
#     def validate_bits_per_sample(value):
#         if value != bits_per_sample:
#             raise cv.Invalid(
#                 f"Component {name} requires {bits_per_sample} bits per sample"
#             )
#         return value

#     def validate_channels(value):
#         if value != channels:
#             raise cv.Invalid(f"Component {name} requires {channels} channels")
#         return value

#     def validate_sample_rate(value):
#         if value != sample_rate:
#             raise cv.Invalid(f"COmponent {name} requires {sample_rate} sample rate")
#         return value

#     def validate_audio_compatiblity(config):
#         audio_schema = {}
#         audio_schema[cv.Required(CONF_BITS_PER_SAMPLE)] = validate_bits_per_sample
#         audio_schema[cv.Required(CONF_CHANNELS)] = validate_channels
#         audio_schema[cv.Required(CONF_SAMPLE_RATE)] = validate_sample_rate

#     return validate_audio_compatiblity


async def to_code(config):
    cg.add_library(
        None,
        None,
        "https://github.com/kahrendt/esp-audio-libs.git#quantization-utils",
    )
