import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["audio"]
CODEOWNERS = ["kahrendt"]
DEPENDENCIES = ["speaker"]

mixer_speaker_ns = cg.esphome_ns.namespace("mixer_speaker")
MixerSpeaker = mixer_speaker_ns.class_("MixerSpeaker", cg.Component, speaker.Speaker)
InputSpeaker = mixer_speaker_ns.class_("InputSpeaker", cg.Component, speaker.Speaker)

CONF_PRIMARY_SPEAKER = "primary_speaker"
CONF_OUTPUT_SPEAKER = "output_speaker"
CONF_SECONDARY_SPEAKER = "secondary_speaker"

INPUT_SPEAKER_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {cv.GenerateID(): cv.declare_id(InputSpeaker)}
)

CONFIG_SCHEMA = cv.All(
    {
        cv.GenerateID(): cv.declare_id(MixerSpeaker),
        cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_PRIMARY_SPEAKER): INPUT_SPEAKER_SCHEMA,
        cv.Required(CONF_SECONDARY_SPEAKER): INPUT_SPEAKER_SCHEMA,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(spkr))

    primary_speaker_config = config[CONF_PRIMARY_SPEAKER]
    announce_speaker = cg.new_Pvariable(primary_speaker_config[CONF_ID])
    await cg.register_component(announce_speaker, primary_speaker_config)
    await cg.register_parented(announce_speaker, config[CONF_ID])
    await speaker.register_speaker(announce_speaker, primary_speaker_config)
    cg.add(var.set_primary_speaker(announce_speaker))

    secondary_speaker_config = config[CONF_SECONDARY_SPEAKER]
    secondary_speaker = cg.new_Pvariable(secondary_speaker_config[CONF_ID])
    await cg.register_component(secondary_speaker, secondary_speaker_config)
    await cg.register_parented(secondary_speaker, config[CONF_ID])
    await speaker.register_speaker(secondary_speaker, secondary_speaker_config)
    cg.add(var.set_secondary_speaker(secondary_speaker))
