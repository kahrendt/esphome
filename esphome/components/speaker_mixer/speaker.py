from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_DURATION,
    CONF_ID,
    CONF_NEVER,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
    CONF_TASK_STACK_IN_PSRAM,
    CONF_TIMEOUT,
    PLATFORM_ESP32,
)
from esphome.core.entity_helpers import inherit_property_from
import esphome.final_validate as fv

AUTO_LOAD = ["audio"]
CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["speaker"]

speaker_mixer_ns = cg.esphome_ns.namespace("speaker_mixer")
SpeakerMixer = speaker_mixer_ns.class_("SpeakerMixer", cg.Component, speaker.Speaker)
SourceSpeaker = speaker_mixer_ns.class_("SourceSpeaker", cg.Component, speaker.Speaker)

CONF_DECIBEL_REDUCTION = "decibel_reduction"
CONF_QUEUE_MODE = "queue_mode"
CONF_SOURCE_SPEAKERS = "source_speakers"

DuckingApplyAction = speaker_mixer_ns.class_(
    "DuckingApplyAction", automation.Action, cg.Parented.template(SourceSpeaker)
)


SOURCE_SPEAKER_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(SourceSpeaker),
        cv.Optional(
            CONF_BUFFER_DURATION, default="100ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TIMEOUT, default="500ms"): cv.Any(
            cv.positive_time_period_milliseconds,
            cv.one_of(CONF_NEVER, lower=True),
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SpeakerMixer),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_SOURCE_SPEAKERS): cv.All(
                cv.ensure_list(SOURCE_SPEAKER_SCHEMA), cv.Length(min=2, max=8)
            ),
            cv.Optional(CONF_NUM_CHANNELS): cv.int_range(min=1, max=2),
            cv.Optional(CONF_QUEUE_MODE, default=False): cv.boolean,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=False): cv.boolean,
        }
    ),
    cv.only_on([PLATFORM_ESP32]),
)


def inherit_property_from_id(property_to_inherit, parent_id):
    """Validator that inherits a configuration property from another entity, for use with FINAL_VALIDATE_SCHEMA.
    If a property is already set, it will not be inherited.
    Keyword arguments:
    property_to_inherit -- the name or path of the property to inherit, e.g. CONF_ICON or [CONF_SENSOR, 0, CONF_ICON]
                           (the parent must exist, otherwise nothing is done).
    parent_id -- the ID of the parent from which the property is inherited.
    """

    def _walk_config(config, path):
        walk = [path] if not isinstance(path, list) else path
        for item_or_index in walk:
            config = config[item_or_index]
        return config

    def inherit_property(config):
        # Split the property into its path and name
        if not isinstance(property_to_inherit, list):
            property_path, property = [], property_to_inherit
        else:
            property_path, property = property_to_inherit[:-1], property_to_inherit[-1]

        # Check if the property to inherit is accessible
        try:
            config_part = _walk_config(config, property_path)
        except KeyError:
            return config

        # Only inherit the property if it does not exist yet
        if property not in config_part:
            fconf = fv.full_config.get()

            # Get config for the parent entity
            # parent_id = _walk_config(config, parent_id_property)
            parent_path = fconf.get_path_for_id(parent_id)[:-1]
            parent_config = fconf.get_config_for_path(parent_path)

            # If parent sensor has the property set, inherit it
            if property in parent_config:
                path = fconf.get_path_for_id(config[CONF_ID])[:-1]
                this_config = _walk_config(
                    fconf.get_config_for_path(path), property_path
                )
                value = parent_config[property]
                this_config[property] = value

        return config

    return inherit_property


def validate_source_speaker(config):
    fconf = fv.full_config.get()

    # Get ID for source sensor
    path = fconf.get_path_for_id(config[CONF_ID])[:-3]
    path.append(CONF_OUTPUT_SPEAKER)
    output_speaker_id = fconf.get_config_for_path(path)
    print(output_speaker_id)

    for property in [CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE]:
        inherit_function = inherit_property_from_id(
            property,
            output_speaker_id,
        )
        inherit_function(config)
        print(property, config[property])
    return config


FINAL_VALIDATE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_SOURCE_SPEAKERS): [validate_source_speaker],
        },
        extra=cv.ALLOW_EXTRA,
    ),
    inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER),
)
# FINAL_VALIDATE_SCHEMA = cv.All(
#     # cv.Schema(
#     #     {
#     #         cv.Optional(CONF_BITS_PER_SAMPLE): cv.int_range(8, 32),
#     #         cv.Optional(CONF_NUM_CHANNELS): cv.int_range(1, 2),
#     #         cv.Optional(CONF_SAMPLE_RATE): cv.int_range(8000, 48000),
#     #     },
#     #     extra=cv.ALLOW_EXTRA,
#     # ),
#     inherit_property_from(CONF_BITS_PER_SAMPLE, CONF_OUTPUT_SPEAKER),
#     inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER),
#     inherit_property_from(CONF_SAMPLE_RATE, CONF_OUTPUT_SPEAKER),
# )


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])

    cg.add(var.set_output_channels(config[CONF_NUM_CHANNELS]))
    cg.add(var.set_output_speaker(spkr))
    cg.add(var.set_queue_mode(config[CONF_QUEUE_MODE]))

    cg.add(var.set_task_stack_in_psram(config[CONF_TASK_STACK_IN_PSRAM]))
    if config[CONF_TASK_STACK_IN_PSRAM]:
        esp32.add_idf_sdkconfig_option(
            "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", True
        )

    for speaker_config in config[CONF_SOURCE_SPEAKERS]:
        source_speaker = cg.new_Pvariable(speaker_config[CONF_ID])

        cg.add(source_speaker.set_buffer_duration(speaker_config[CONF_BUFFER_DURATION]))

        if speaker_config[CONF_TIMEOUT] != CONF_NEVER:
            cg.add(source_speaker.set_timeout(speaker_config[CONF_TIMEOUT]))

        await cg.register_component(source_speaker, speaker_config)
        await cg.register_parented(source_speaker, config[CONF_ID])
        await speaker.register_speaker(source_speaker, speaker_config)

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
