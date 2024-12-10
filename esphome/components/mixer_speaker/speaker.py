from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_DURATION, CONF_ID

AUTO_LOAD = ["audio"]
CODEOWNERS = ["kahrendt"]
DEPENDENCIES = ["speaker"]

mixer_speaker_ns = cg.esphome_ns.namespace("mixer_speaker")
MixerSpeaker = mixer_speaker_ns.class_("MixerSpeaker", cg.Component, speaker.Speaker)
InputSpeaker = mixer_speaker_ns.class_("InputSpeaker", cg.Component, speaker.Speaker)

CONF_DECIBEL_REDUCTION = "decibel_reduction"
CONF_PRIMARY_SPEAKER = "primary_speaker"
CONF_OUTPUT_SPEAKER = "output_speaker"
CONF_SECONDARY_SPEAKER = "secondary_speaker"

DuckingSetAction = mixer_speaker_ns.class_(
    "DuckingSetAction", automation.Action, cg.Parented.template(InputSpeaker)
)


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


@automation.register_action(
    "mixer_input_speaker.set_ducking",
    DuckingSetAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(InputSpeaker),
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
