import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SAMPLE_RATE

AUTO_LOAD = ["audio"]
CODEOWNERS = ["kahrendt"]
DEPENDENCIES = ["speaker"]

resampling_speaker_ns = cg.esphome_ns.namespace("resampling_speaker")
ResamplingSpeaker = resampling_speaker_ns.class_(
    "ResamplingSpeaker", cg.Component, speaker.Speaker
)

CONF_OUTPUT_SPEAKER = "output_speaker"
CONF_NEVER = "never"


CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(ResamplingSpeaker),
        cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=1),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    cg.add(var.set_target_sample_rate(config[CONF_SAMPLE_RATE]))
