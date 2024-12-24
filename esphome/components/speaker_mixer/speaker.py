from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_DURATION, CONF_ID, CONF_NUM_CHANNELS, CONF_TIMEOUT

AUTO_LOAD = ["audio"]
CODEOWNERS = ["kahrendt"]
DEPENDENCIES = ["speaker"]

speaker_mixer_ns = cg.esphome_ns.namespace("speaker_mixer")
SpeakerMixer = speaker_mixer_ns.class_("SpeakerMixer", cg.Component, speaker.Speaker)
SourceSpeaker = speaker_mixer_ns.class_("SourceSpeaker", cg.Component, speaker.Speaker)

CONF_DECIBEL_REDUCTION = "decibel_reduction"
CONF_OUTPUT_SPEAKER = "output_speaker"
CONF_NEVER = "never"
CONF_SOURCE_SPEAKERS = "source_speakers"

DuckingApplyAction = speaker_mixer_ns.class_(
    "DuckingApplyAction", automation.Action, cg.Parented.template(SourceSpeaker)
)


SOURCE_SPEAKER_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(SourceSpeaker),
        cv.Optional(CONF_TIMEOUT, default="500ms"): cv.Any(
            cv.positive_time_period_milliseconds,
            cv.one_of(CONF_NEVER, lower=True),
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    {
        cv.GenerateID(): cv.declare_id(SpeakerMixer),
        cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_SOURCE_SPEAKERS): cv.All(
            cv.ensure_list(SOURCE_SPEAKER_SCHEMA), cv.Length(min=2, max=2)
        ),
        cv.Optional(CONF_NUM_CHANNELS, default=2): cv.int_range(min=1, max=2),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(spkr))

    for speaker_config in config[CONF_SOURCE_SPEAKERS]:
        source_speaker = cg.new_Pvariable(speaker_config[CONF_ID])

        if speaker_config[CONF_TIMEOUT] != CONF_NEVER:
            cg.add(source_speaker.set_timeout(speaker_config[CONF_TIMEOUT]))

        await cg.register_component(source_speaker, speaker_config)
        await cg.register_parented(source_speaker, config[CONF_ID])
        await speaker.register_speaker(source_speaker, speaker_config)

        cg.add(var.set_output_channels(config[CONF_NUM_CHANNELS]))
        cg.add(var.add_source_speaker(source_speaker))


@automation.register_action(
    "speaker_mixer.apply_ducking",
    DuckingApplyAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SourceSpeaker),
            cv.Required(CONF_DECIBEL_REDUCTION): cv.templatable(
                cv.int_range(min=0, max=51)
            ),
            cv.Optional(CONF_DURATION, default="0.0s"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
)
async def ducking_set_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    decibel_reduction = await cg.templatable(
        config[CONF_DECIBEL_REDUCTION], args, cg.uint8
    )
    cg.add(var.set_decibel_reduction(decibel_reduction))
    duration = await cg.templatable(config[CONF_DURATION], args, cg.uint32)
    cg.add(var.set_duration(duration))
    return var
