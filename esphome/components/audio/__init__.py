import esphome.codegen as cg
import esphome.config_validation as cv

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


async def to_code(config):
    cg.add_library(
        None,
        None,
        "https://github.com/kahrendt/esp-audio-libs.git#resampling-dither",
    )
