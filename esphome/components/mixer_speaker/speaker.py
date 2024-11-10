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

CONF_MEDIA_SPEAKER = "media_speaker"
CONF_ANNOUNCEMENT_SPEAKER = "announcement_speaker"
CONF_OUTPUT_SPEAKER = "output_speaker"

INPUT_SPEAKER_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {cv.GenerateID(): cv.declare_id(InputSpeaker)}
)

CONFIG_SCHEMA = cv.All(
    {
        cv.GenerateID(): cv.declare_id(MixerSpeaker),
        cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_ANNOUNCEMENT_SPEAKER): INPUT_SPEAKER_SCHEMA,
        cv.Required(CONF_MEDIA_SPEAKER): INPUT_SPEAKER_SCHEMA,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(spkr))

    announcement_speaker_config = config[CONF_ANNOUNCEMENT_SPEAKER]
    announce_speaker = cg.new_Pvariable(announcement_speaker_config[CONF_ID])
    await cg.register_component(announce_speaker, announcement_speaker_config)
    await cg.register_parented(announce_speaker, config[CONF_ID])
    await speaker.register_speaker(announce_speaker, announcement_speaker_config)
    cg.add(var.set_announcement_speaker(announce_speaker))

    media_speaker_config = config[CONF_MEDIA_SPEAKER]
    media_speaker = cg.new_Pvariable(media_speaker_config[CONF_ID])
    await cg.register_component(media_speaker, media_speaker_config)
    await cg.register_parented(media_speaker, config[CONF_ID])
    await speaker.register_speaker(media_speaker, media_speaker_config)
    cg.add(var.set_media_speaker(media_speaker))
