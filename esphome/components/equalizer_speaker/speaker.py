import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["audio"]
CODEOWNERS = ["kahrendt"]
DEPENDENCIES = ["speaker"]

equalizer_speaker_ns = cg.esphome_ns.namespace("equalizer_speaker")
EqualizerSpeaker = equalizer_speaker_ns.class_(
    "EqualizerSpeaker", cg.Component, speaker.Speaker
)

CONF_OUTPUT_SPEAKER = "output_speaker"

CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(EqualizerSpeaker),
        cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))
