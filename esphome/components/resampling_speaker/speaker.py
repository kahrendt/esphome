import esphome.codegen as cg
from esphome.components import esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_FILTERS,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
    CONF_TASK_STACK_IN_PSRAM,
    PLATFORM_ESP32,
)
from esphome.core.entity_helpers import inherit_property_from

AUTO_LOAD = ["audio"]
CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["speaker"]

resampling_speaker_ns = cg.esphome_ns.namespace("resampling_speaker")
ResamplingSpeaker = resampling_speaker_ns.class_(
    "ResamplingSpeaker", cg.Component, speaker.Speaker
)

CONF_TAPS = "taps"


def _validate_taps(taps):
    value = cv.int_range(min=16, max=128)(taps)
    if value % 4 != 0:
        raise cv.Invalid("Number of taps must be divisible by 4")
    return value


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ResamplingSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(
                CONF_BUFFER_DURATION, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=False): cv.boolean,
            cv.Optional(CONF_FILTERS, default=16): cv.int_range(min=2, max=1024),
            cv.Optional(CONF_TAPS, default=16): _validate_taps,
            cv.Optional(CONF_BITS_PER_SAMPLE): cv.int_range(8, 32),
            cv.Optional(CONF_SAMPLE_RATE): cv.int_range(8000, 48000),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)

FINAL_VALIDATE_SCHEMA = cv.All(
    # cv.Schema(
    #     {
    #         cv.Optional(CONF_BITS_PER_SAMPLE): cv.int_range(8, 32),
    #         cv.Optional(CONF_NUM_CHANNELS): cv.int_range(1, 2),
    #         cv.Optional(CONF_SAMPLE_RATE): cv.int_range(8000, 48000),
    #     },
    #     extra=cv.ALLOW_EXTRA,
    # ),
    inherit_property_from(CONF_BITS_PER_SAMPLE, CONF_OUTPUT_SPEAKER),
    inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER),
    inherit_property_from(CONF_SAMPLE_RATE, CONF_OUTPUT_SPEAKER),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))

    cg.add(var.set_task_stack_in_psram(config[CONF_TASK_STACK_IN_PSRAM]))
    if config[CONF_TASK_STACK_IN_PSRAM]:
        esp32.add_idf_sdkconfig_option(
            "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY", True
        )

    cg.add(var.set_target_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_target_sample_rate(config[CONF_SAMPLE_RATE]))

    cg.add(var.set_filters(config[CONF_FILTERS]))
    cg.add(var.set_taps(config[CONF_TAPS]))
